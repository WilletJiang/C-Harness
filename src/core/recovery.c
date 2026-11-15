#include "core.h"

static ah_code recover_operations(ah_engine *e, const char *sid, ah_state *state, ah_error *error) {
    sqlite3_stmt *stmt = NULL;
    ah_code code = core_prepare(e,
                                "SELECT id FROM operations WHERE session_id=? AND status!='done' "
                                "ORDER BY seq",
                                &stmt, error);
    if (code != AH_OK)
        return code;
    sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_TRANSIENT);
    bool blocked = false;
    int step = SQLITE_DONE;
    while (code == AH_OK && (step = sqlite3_step(stmt)) == SQLITE_ROW) {
        core_operation op;
        code = core_get_operation(e, sid, (const char *)sqlite3_column_text(stmt, 0), &op, error);
        if (code != AH_OK)
            break;
        bool executing = !strcmp(op.status, "executing");
        if (!strcmp(op.status, "unknown") || (executing && op.effect != AH_EFFECT_READ)) {
            blocked = true;
            *state = AH_OUTCOME_UNKNOWN;
            if (executing) {
                code = core_set_operation(e, sid, op.id, "unknown", error);
                if (code == AH_OK)
                    code = core_event(e, sid, op.id, "tool.outcome_unknown",
                                      "{\"reason\":\"execution interrupted\"}", error);
            }
        } else if (executing) {
            const char *result = "{\"error\":\"read interrupted by process exit\"}";
            code = blocked ? core_save_result(e, sid, &op, result, true, "received",
                                              "tool.received", error)
                           : core_receipt(e, sid, &op, result, true, "tool.interrupted", error);
        } else if (!strcmp(op.status, "received") || !strcmp(op.status, "received_late") ||
                   !strcmp(op.status, "resolved")) {
            if (!blocked)
                code = core_receipt(e, sid, &op, op.result, op.is_error,
                                    !strcmp(op.status, "received_late") ? "tool.late_result"
                                                                        : "tool.completed",
                                    error);
        } else
            blocked = true;
        core_operation_free(&op);
    }
    sqlite3_finalize(stmt);
    if (code == AH_OK && step != SQLITE_DONE)
        code = ah_fail(error, AH_IO, "cannot recover operations");
    return code;
}

ah_code core_recover(ah_engine *e, ah_error *err) {
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(e,
                              "SELECT id FROM sessions WHERE state=? OR EXISTS "
                              "(SELECT 1 FROM operations WHERE session_id=sessions.id "
                              "AND status='executing')",
                              &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_int(s, 1, AH_RUNNING);
    char **ids = NULL;
    size_t count = 0;
    int step;
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        char **next = realloc(ids, (count + 1) * sizeof(*ids));
        if (!next) {
            rc = ah_fail(err, AH_IO, "out of memory");
            break;
        }
        ids = next;
        ids[count] = ah_strdup((const char *)sqlite3_column_text(s, 0));
        if (!ids[count]) {
            rc = ah_fail(err, AH_IO, "out of memory");
            break;
        }
        count++;
    }
    sqlite3_finalize(s);
    if (rc == AH_OK && step != SQLITE_DONE)
        rc = ah_fail(err, AH_IO, "recovery scan failed");
    for (size_t i = 0; i < count && rc == AH_OK; i++) {
        uint64_t after = 0;
        rc = core_begin(e, &after, err);
        ah_state state = AH_STOPPED;
        if (rc == AH_OK)
            rc = recover_operations(e, ids[i], &state, err);
        if (rc == AH_OK)
            rc = core_state(e, ids[i], state, err);
        if (rc == AH_OK)
            rc = core_event(e, ids[i], NULL, "session.recovered", "{}", err);
        if (rc == AH_OK)
            rc = core_commit(e, after, err);
        else
            core_rollback(e);
    }
    for (size_t i = 0; i < count; i++)
        free(ids[i]);
    free(ids);
    return rc;
}
