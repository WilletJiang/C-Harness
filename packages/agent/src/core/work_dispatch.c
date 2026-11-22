#include "work.h"

static ah_code blocked(ah_engine *e, core_session *session, core_operation *op, const char *reason,
                       bool *paused, ah_error *error) {
    *paused = true;
    if (session->state == AH_WAITING_WORK)
        return AH_OK;
    char *json = core_object_string("reason", reason);
    if (!json)
        return ah_fail(error, AH_IO, "cannot encode blocked work");
    uint64_t after = 0;
    ah_code code = core_begin(e, &after, error);
    if (code == AH_OK)
        code = core_state(e, session->id, AH_WAITING_WORK, error);
    if (code == AH_OK && session->state != AH_WAITING_WORK)
        code = core_event(e, session->id, op->id, "work.blocked", json, error);
    if (code == AH_OK)
        code = core_commit(e, after, error);
    else
        core_rollback(e);
    free(json);
    if (code == AH_OK)
        session->state = AH_WAITING_WORK;
    return code;
}

static ah_code replay_revoked(ah_engine *e, core_session *session, core_operation *op, bool *paused,
                              ah_error *error) {
    *paused = true;
    bool external = op->effect != AH_EFFECT_READ;
    uint64_t after = 0;
    ah_code code = core_begin(e, &after, error);
    if (code == AH_OK && external)
        code = core_set_operation(e, session->id, op->id, "unknown", error);
    if (code == AH_OK && external)
        code = core_event(e, session->id, op->id, "tool.outcome_unknown",
                          "{\"reason\":\"interrupted step no longer permits replay\"}", error);
    if (code == AH_OK && external)
        code = core_state(e, session->id, AH_OUTCOME_UNKNOWN, error);
    if (code == AH_OK && !external) {
        op->result_format = AH_TOOL_RESULT_JSON;
        code = core_save_result(e, session->id, op,
                                "{\"error\":\"interrupted step no longer permits replay\"}", true,
                                "received", "tool.received", error);
    }
    if (code == AH_OK)
        code = core_commit(e, after, error);
    else
        core_rollback(e);
    if (code == AH_OK && external) {
        session->state = AH_OUTCOME_UNKNOWN;
        code = ah_fail(error, AH_UNCERTAIN, "interrupted effect requires explicit resolution");
    }
    return code;
}

/* Registration happens after opening storage. Compatibility is checked before
 * every dispatch, including the first invocation after recovery. */
ah_code core_work_prepare(ah_engine *e, core_session *session, core_operation *op, core_tool *tool,
                          bool *paused, ah_error *error) {
    *paused = false;
    core_work work;
    ah_code code = core_work_load(e, session->id, op->id, &work, error);
    if (code == AH_NOT_FOUND)
        return tool && tool->work
                   ? blocked(e, session, op, "durable policy changed since intent", paused, error)
                   : AH_OK;
    if (code != AH_OK)
        return code;
    if (!tool || !tool->work)
        code = blocked(e, session, op, "durable definition unavailable", paused, error);
    else if (tool->tool.effect != op->effect || tool->tool.requires_approval != op->approval ||
             !op->schema || strcmp(tool->schema, op->schema))
        code = blocked(e, session, op, "durable tool policy changed", paused, error);
    else if (work.restarts &&
             (work.replay != AH_REPLAY_SAFE || tool->work->replay != AH_REPLAY_SAFE))
        code = replay_revoked(e, session, op, paused, error);
    else if (work.version != tool->work->version) {
        if (work.version > tool->work->version || !tool->work->migrate)
            code = blocked(e, session, op, "checkpoint version unavailable", paused, error);
        else {
            char *checkpoint = NULL;
            ah_error migration = {0};
            ah_code migrated = tool->work->migrate(
                tool->tool.context, work.version, op->arguments,
                work.checkpoint_json ? work.checkpoint_json : "{}", &checkpoint, &migration);
            yyjson_doc *doc = migrated == AH_OK && checkpoint &&
                                      strlen(checkpoint) <= CORE_WORK_JSON_LIMIT &&
                                      ah_json_valid(checkpoint)
                                  ? yyjson_read(checkpoint, strlen(checkpoint), 0)
                                  : NULL;
            if (!doc || !yyjson_is_obj(yyjson_doc_get_root(doc)))
                code = blocked(e, session, op, "checkpoint migration failed", paused, error);
            else {
                uint64_t after = 0;
                code = core_begin(e, &after, error);
                sqlite3_stmt *stmt = NULL;
                if (code == AH_OK)
                    code = core_prepare(e,
                                        "UPDATE operation_work SET version=?,checkpoint_json=? "
                                        "WHERE operation_id=? AND version=?",
                                        &stmt, error);
                if (code == AH_OK) {
                    sqlite3_bind_int64(stmt, 1, tool->work->version);
                    sqlite3_bind_text(stmt, 2, checkpoint, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 3, op->id, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(stmt, 4, work.version);
                    code = core_done(e, stmt, error);
                }
                if (code == AH_OK)
                    code = core_event(e, session->id, op->id, "work.migrated", "{}", error);
                if (code == AH_OK)
                    code = core_commit(e, after, error);
                else
                    core_rollback(e);
            }
            yyjson_doc_free(doc);
            free(checkpoint);
        }
    }
    core_work_free(&work);
    return code;
}

/* A parked parent stays ordered in the model exchange while its owned children
 * run through the same executor. Only the bounded child frontier is inspected. */
ah_code core_work_frontier(ah_engine *e, const char *sid, core_operation *op, unsigned depth,
                           bool *ready, ah_error *error) {
    *ready = true;
    if (strcmp(op->status, "parked"))
        return AH_OK;
    if (depth > 8)
        return ah_fail(error, AH_LIMIT, "durable call depth exceeded");
    ah_code code = core_work_ready(e, sid, op->id, ready, error);
    if (code != AH_OK || *ready)
        return code;
    sqlite3_stmt *stmt = NULL;
    code = core_prepare(e,
                        "SELECT o.id,EXISTS(SELECT 1 FROM work_calls nested WHERE parent_id=o.id) "
                        "FROM work_calls c JOIN operations o ON o.id=c.operation_id "
                        "WHERE c.parent_id=? AND o.session_id=? AND o.status!='done' "
                        "ORDER BY o.seq LIMIT 33",
                        &stmt, error);
    if (code != AH_OK)
        return code;
    sqlite3_bind_text(stmt, 1, op->id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, sid, -1, SQLITE_TRANSIENT);
    int step = SQLITE_DONE;
    unsigned inspected = 0;
    bool bypassed = false;
    while (code == AH_OK && (step = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (++inspected > 32) {
            code = ah_fail(error, AH_LIMIT, "durable child frontier exceeds bound");
            break;
        }
        core_operation child;
        code =
            core_get_operation(e, sid, (const char *)sqlite3_column_text(stmt, 0), &child, error);
        if (code != AH_OK)
            break;
        bool leaf = sqlite3_column_int(stmt, 1) == 0;
        code = core_work_frontier(e, sid, &child, depth + 1, ready, error);
        bool read = child.effect == AH_EFFECT_READ && !child.approval;
        if (bypassed && !read)
            *ready = false;
        if (code == AH_OK && *ready) {
            core_operation_free(op);
            *op = child;
            break;
        }
        core_operation_free(&child);
        /* A suspended read may release capacity to a later read. Effectful
         * calls and subtrees retain their ordering fence while suspended. */
        if (!read || !leaf)
            break;
        bypassed = true;
    }
    if (code == AH_OK && step != SQLITE_DONE && step != SQLITE_ROW)
        code = ah_fail(error, AH_IO, "cannot read child work frontier");
    sqlite3_finalize(stmt);
    return code;
}

ah_code core_work_has_children(ah_engine *e, const char *id, bool *children, ah_error *error) {
    *children = false;
    sqlite3_stmt *row = NULL;
    ah_code code =
        core_prepare(e, "SELECT EXISTS(SELECT 1 FROM work_calls WHERE parent_id=?)", &row, error);
    if (code == AH_OK) {
        sqlite3_bind_text(row, 1, id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(row) == SQLITE_ROW)
            *children = sqlite3_column_int(row, 0) != 0;
        else
            code = ah_fail(error, AH_IO, "cannot inspect owned work children");
    }
    sqlite3_finalize(row);
    return code;
}

/* A parked independent leaf read releases its slot to later independent roots.
 * A later root's children remain in its ordering scope, and visible receipts
 * still publish in the original model-call order. */
ah_code core_work_select(ah_engine *e, const char *sid, core_operation *op, bool *ready,
                         ah_error *error) {
    ah_code code = core_work_frontier(e, sid, op, 0, ready, error);
    if (code != AH_OK || *ready || !core_independent_read(e, op))
        return code;
    bool children = false;
    code = core_work_has_children(e, op->id, &children, error);
    if (code != AH_OK || children)
        return code;
    sqlite3_stmt *row = NULL;
    code = core_prepare(e,
                        "SELECT o.id,EXISTS(SELECT 1 FROM work_calls c WHERE c.parent_id=o.id) "
                        "FROM operations o WHERE o.session_id=?1 AND o.status!='done' "
                        "AND o.seq>(SELECT seq FROM operations WHERE id=?2) "
                        "AND NOT EXISTS(SELECT 1 FROM work_calls c WHERE c.operation_id=o.id "
                        "AND c.parent_id IS NOT NULL) "
                        "AND (o.status!='parked' OR o.effect!=0 OR o.approval!=0 "
                        "OR EXISTS(SELECT 1 FROM work_calls c WHERE c.parent_id=o.id) "
                        "OR EXISTS(SELECT 1 FROM operation_work w WHERE w.operation_id=o.id "
                        "AND w.wake_at_ms>0 AND w.wake_at_ms<=?3)) ORDER BY o.seq",
                        &row, error);
    if (code != AH_OK)
        return code;
    sqlite3_bind_text(row, 1, sid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(row, 2, op->id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(row, 3, (sqlite3_int64)e->work_clock_ms());
    int step = SQLITE_DONE;
    while ((step = sqlite3_step(row)) == SQLITE_ROW) {
        if (core_cancelled(e)) {
            code = ah_fail(error,
                           atomic_load_explicit(&e->cancelled, memory_order_relaxed) ? AH_CANCELLED
                                                                                     : AH_LIMIT,
                           "work frontier interrupted");
            break;
        }
        core_operation candidate;
        code = core_get_operation(e, sid, (const char *)sqlite3_column_text(row, 0), &candidate,
                                  error);
        if (code != AH_OK)
            break;
        bool leaf = sqlite3_column_int(row, 1) == 0;
        if (!core_independent_read(e, &candidate)) {
            core_operation_free(&candidate);
            break;
        }
        bool available = false;
        code = core_work_frontier(e, sid, &candidate, 0, &available, error);
        bool visible_receipt = !candidate.hidden && (!strcmp(candidate.status, "received") ||
                                                     !strcmp(candidate.status, "received_late") ||
                                                     !strcmp(candidate.status, "resolved"));
        if (code == AH_OK && available && !visible_receipt &&
            core_independent_read(e, &candidate)) {
            core_operation_free(op);
            *op = candidate;
            *ready = true;
            break;
        }
        core_operation_free(&candidate);
        if (code != AH_OK || !leaf || (available && !visible_receipt))
            break;
    }
    if (code == AH_OK && step != SQLITE_DONE && step != SQLITE_ROW)
        code = ah_fail(error, AH_IO, "cannot read later work roots");
    sqlite3_finalize(row);
    return code;
}
