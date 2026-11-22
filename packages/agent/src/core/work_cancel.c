#include "work.h"
#include <stdio.h>

static ah_code blocked_descendant(ah_engine *e, const char *id, bool *blocked, ah_error *error) {
    sqlite3_stmt *row = NULL;
    ah_code code = core_prepare(
        e,
        "WITH RECURSIVE tree(id) AS (SELECT operation_id FROM work_calls WHERE parent_id=?1 "
        "UNION ALL SELECT w.operation_id FROM work_calls w JOIN tree t ON w.parent_id=t.id) "
        "SELECT EXISTS(SELECT 1 FROM tree t JOIN operations o ON o.id=t.id "
        "WHERE o.status IN('executing','unknown'))",
        &row, error);
    if (code == AH_OK) {
        sqlite3_bind_text(row, 1, id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(row) == SQLITE_ROW)
            *blocked = sqlite3_column_int(row, 0) != 0;
        else
            code = ah_fail(error, AH_IO, "cannot inspect unsettled child effects");
    }
    sqlite3_finalize(row);
    return code;
}

static ah_code cancel_operation(ah_engine *e, const char *sid, const char *id, bool *busy,
                                bool *unknown, ah_error *error) {
    core_operation operation;
    ah_code code = core_get_operation(e, sid, id, &operation, error);
    if (code != AH_OK)
        return code;
    bool executing = !strcmp(operation.status, "executing");
    if (!strcmp(operation.status, "done")) {
        core_operation_free(&operation);
        return AH_OK;
    }
    if (!strcmp(operation.status, "unknown"))
        *unknown = true;
    else if (operation.result &&
             (!strcmp(operation.status, "received") || !strcmp(operation.status, "received_late") ||
              !strcmp(operation.status, "resolved")))
        code = core_receipt(e, sid, &operation, operation.result, operation.is_error,
                            "tool.completed", error);
    else if (executing && operation.effect != AH_EFFECT_READ) {
        core_jobs_cancel_owned(e, id, NULL);
        code = core_set_operation(e, sid, id, "unknown", error);
        if (code == AH_OK)
            code = core_event(e, sid, id, "tool.outcome_unknown",
                              "{\"reason\":\"work cancelled during effect execution\"}", error);
        *unknown = true;
    } else if (executing && core_jobs_cancel_owned(e, id, NULL) == AH_OK) {
        *busy = true;
        code = core_event(e, sid, id, "work.cancel_requested", "{}", error);
    } else {
        operation.result_format = AH_TOOL_RESULT_JSON;
        code = core_receipt(e, sid, &operation, "{\"error\":\"work cancelled\"}", true,
                            "work.cancelled", error);
    }
    core_operation_free(&operation);
    return code;
}

static ah_code defer_root_cancellation(ah_engine *e, const char *sid, const char *id, bool *unknown,
                                       ah_error *error) {
    core_operation operation;
    ah_code code = core_get_operation(e, sid, id, &operation, error);
    if (code != AH_OK)
        return code;
    bool settled = !strcmp(operation.status, "done") || !strcmp(operation.status, "unknown") ||
                   !strcmp(operation.status, "received") ||
                   !strcmp(operation.status, "received_late") ||
                   !strcmp(operation.status, "resolved");
    if (!strcmp(operation.status, "executing") && operation.effect != AH_EFFECT_READ) {
        *unknown = true;
        code = core_set_operation(e, sid, id, "unknown", error);
        if (code == AH_OK)
            code = core_event(e, sid, id, "tool.outcome_unknown",
                              "{\"reason\":\"work cancelled during effect execution\"}", error);
    } else if (!settled) {
        operation.result_format = AH_TOOL_RESULT_JSON;
        code = core_save_result(e, sid, &operation, "{\"error\":\"work cancelled\"}", true,
                                "received", "work.cancel_requested", error);
    }
    core_operation_free(&operation);
    return code;
}

static ah_code cancel_tree(ah_engine *e, const char *sid, const char *id, bool include_root,
                           ah_error *error) {
    /* Copy identifiers before changing the scanned tables. Child bounds also
     * bound this owner-thread cleanup and make malformed stored cycles fail. */
    char ids[128][AH_ID_CAP];
    unsigned count = 0;
    sqlite3_stmt *row = NULL;
    ah_code code = core_prepare(
        e,
        "WITH RECURSIVE tree(id,depth) AS (SELECT ?1,0 UNION ALL "
        "SELECT w.operation_id,t.depth+1 FROM work_calls w JOIN tree t "
        "ON w.parent_id=t.id WHERE t.depth<8) "
        "SELECT o.id FROM tree t JOIN operations o ON o.id=t.id "
        "WHERE o.session_id=?2 AND (?3 OR t.depth>0) AND (t.depth=0 OR o.status!='done') "
        "ORDER BY t.depth DESC,o.seq",
        &row, error);
    int step = SQLITE_DONE;
    if (code == AH_OK) {
        sqlite3_bind_text(row, 1, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(row, 2, sid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(row, 3, include_root);
        while ((step = sqlite3_step(row)) == SQLITE_ROW) {
            if (count == 128) {
                code = ah_fail(error, AH_LIMIT, "work cancellation tree exceeds bound");
                break;
            }
            snprintf(ids[count++], AH_ID_CAP, "%s", (const char *)sqlite3_column_text(row, 0));
        }
        if (code == AH_OK && step != SQLITE_DONE)
            code = ah_fail(error, AH_IO, "cannot inspect work cancellation tree");
    }
    sqlite3_finalize(row);
    if (code == AH_OK && include_root && !count)
        return ah_fail(error, AH_NOT_FOUND, "work operation unavailable");
    bool busy = false, unknown = false;
    for (unsigned index = 0; index < count && code == AH_OK; index++) {
        /* Ancestors remain resumable until their descendants are drained or
         * reconciled; cancellation cannot certify their external effects. */
        bool blocked = false;
        if (busy || unknown)
            code = blocked_descendant(e, ids[index], &blocked, error);
        if (code != AH_OK || blocked)
            continue;
        code = cancel_operation(e, sid, ids[index], &busy, &unknown, error);
    }
    if (code == AH_OK && include_root && (busy || unknown))
        code = defer_root_cancellation(e, sid, id, &unknown, error);
    if (code != AH_OK)
        return code;
    if (unknown)
        return ah_fail(error, AH_UNCERTAIN, "cancelled work has an uncertain effect to reconcile");
    if (busy)
        return ah_fail(error, AH_BUSY, "cancelled work callbacks must drain before completion");
    return AH_OK;
}

ah_code core_work_cancel_tree(ah_engine *e, const char *sid, const char *id, ah_error *error) {
    return cancel_tree(e, sid, id, true, error);
}

ah_code core_work_finish_children(ah_engine *e, const char *sid, const char *id, ah_error *error) {
    return cancel_tree(e, sid, id, false, error);
}

ah_code ah_work_cancel(ah_engine *e, const char *sid, const char *id, ah_error *error) {
    ah_code code = core_guard(e, error);
    if (code != AH_OK)
        return code;
    if (!sid || !id)
        return ah_fail(error, AH_INVALID, "session and work operation required");
    core_operation operation;
    code = core_get_operation(e, sid, id, &operation, error);
    if (code != AH_OK)
        return code;
    bool done = !strcmp(operation.status, "done");
    core_operation_free(&operation);
    if (done)
        return AH_OK;
    uint64_t after = 0;
    code = core_begin(e, &after, error);
    if (code == AH_OK)
        code = core_work_cancel_tree(e, sid, id, error);
    ah_code outcome = code;
    if (code == AH_UNCERTAIN || code == AH_BUSY)
        code = AH_OK;
    if (code == AH_OK)
        code = core_state(e, sid, outcome == AH_UNCERTAIN ? AH_OUTCOME_UNKNOWN : AH_READY, error);
    if (code == AH_OK)
        code = core_commit(e, after, error);
    else
        core_rollback(e);
    return code == AH_OK ? outcome : code;
}
