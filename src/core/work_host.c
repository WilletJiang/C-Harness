#include "work.h"

ah_code core_work_host_session(ah_engine *e, const char *sid, bool *host, ah_error *error) {
    *host = false;
    if (!e->has_work)
        return AH_OK;
    sqlite3_stmt *row = NULL;
    ah_code code = core_prepare(e, "SELECT 1 FROM work_sessions WHERE session_id=?", &row, error);
    if (code == AH_OK) {
        sqlite3_bind_text(row, 1, sid, -1, SQLITE_TRANSIENT);
        int step = sqlite3_step(row);
        *host = step == SQLITE_ROW;
        if (step != SQLITE_ROW && step != SQLITE_DONE)
            code = ah_fail(error, AH_IO, "cannot inspect host work session");
    }
    sqlite3_finalize(row);
    return code;
}

static ah_code host_boundary(ah_engine *e, const char *sid, bool host, const char *key,
                             bool *exists, ah_error *error) {
    sqlite3_stmt *row = NULL;
    ah_code code = core_prepare(
        e,
        "SELECT EXISTS(SELECT 1 FROM work_calls WHERE session_id=?1 AND parent_id IS NULL "
        "AND key=?2), EXISTS(SELECT 1 FROM items WHERE session_id=?1) OR "
        "EXISTS(SELECT 1 FROM commands WHERE session_id=?1) OR "
        "EXISTS(SELECT 1 FROM tasks WHERE session_id=?1) OR "
        "EXISTS(SELECT 1 FROM agent_nodes WHERE session_id=?1) OR "
        "EXISTS(SELECT 1 FROM operations WHERE session_id=?1) OR "
        "EXISTS(SELECT 1 FROM sessions WHERE id=?1 AND (model_calls!=0 OR tool_calls!=0))",
        &row, error);
    if (code == AH_OK) {
        sqlite3_bind_text(row, 1, sid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(row, 2, key, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(row) != SQLITE_ROW)
            code = ah_fail(error, AH_IO, "cannot inspect host session boundary");
        else {
            *exists = sqlite3_column_int(row, 0) != 0;
            if (!host && sqlite3_column_int(row, 1))
                code = ah_fail(error, AH_INVALID,
                               "host work requires a fresh session without model history");
        }
    }
    sqlite3_finalize(row);
    return code;
}

ah_code ah_work_start(ah_engine *e, const char *sid, const char *key, const char *tool,
                      const char *args, char id[AH_ID_CAP], ah_error *error) {
    if (id)
        id[0] = '\0';
    ah_code code = core_guard(e, error);
    if (code != AH_OK)
        return code;
    if (!sid || !key || !id)
        return ah_fail(error, AH_INVALID, "session, key and operation output required");
    core_session session;
    code = core_load(e, sid, &session, error);
    if (code != AH_OK)
        return code;
    bool host = false, exists = false;
    code = core_work_host_session(e, sid, &host, error);
    if (code == AH_OK)
        code = host_boundary(e, sid, host, key, &exists, error);
    if (code == AH_OK && !exists && session.state == AH_OUTCOME_UNKNOWN)
        code = ah_fail(error, AH_UNCERTAIN, "resolve uncertain work before starting another root");
    uint64_t after = 0;
    if (code == AH_OK)
        code = core_begin(e, &after, error);
    sqlite3_stmt *row = NULL;
    if (code == AH_OK && !host)
        code = core_prepare(e, "INSERT INTO work_sessions(session_id) VALUES(?)", &row, error);
    if (code == AH_OK && !host) {
        sqlite3_bind_text(row, 1, sid, -1, SQLITE_TRANSIENT);
        code = core_done(e, row, error);
    }
    if (code == AH_OK)
        code = core_work_create_call(e, sid, NULL, key, tool, args, id, error);
    if (code == AH_OK && !exists)
        code = core_state(e, sid, AH_READY, error);
    if (code == AH_OK)
        code = core_commit(e, after, error);
    else if (!sqlite3_get_autocommit(e->db))
        core_rollback(e);
    if (code == AH_OK && !exists)
        atomic_store_explicit(&e->cancelled, false, memory_order_relaxed);
    if (code != AH_OK)
        id[0] = '\0';
    core_session_free(&session);
    return code;
}

static ah_code add_children(ah_engine *e, const char *sid, const char *id, yyjson_mut_doc *doc,
                            yyjson_mut_val *root, ah_error *error) {
    yyjson_mut_val *children = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "children", children);
    sqlite3_stmt *row = NULL;
    ah_code code = core_prepare(e,
                                "SELECT w.operation_id,w.key FROM work_calls w JOIN operations o "
                                "ON o.id=w.operation_id WHERE w.session_id=? AND w.parent_id=? "
                                "ORDER BY o.seq",
                                &row, error);
    int step = SQLITE_DONE;
    if (code == AH_OK) {
        sqlite3_bind_text(row, 1, sid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(row, 2, id, -1, SQLITE_TRANSIENT);
        while (code == AH_OK && (step = sqlite3_step(row)) == SQLITE_ROW) {
            core_operation operation;
            code = core_get_operation(e, sid, (const char *)sqlite3_column_text(row, 0), &operation,
                                      error);
            if (code != AH_OK)
                break;
            yyjson_mut_val *child = core_work_value(doc, &operation);
            if (child) {
                yyjson_mut_obj_add_strcpy(doc, child, "key",
                                          (const char *)sqlite3_column_text(row, 1));
                yyjson_mut_arr_append(children, child);
            } else
                code = ah_fail(error, AH_IO, "cannot encode child snapshot");
            core_operation_free(&operation);
        }
        if (code == AH_OK && step != SQLITE_DONE)
            code = ah_fail(error, AH_IO, "cannot inspect child work");
    }
    sqlite3_finalize(row);
    return code;
}

ah_code ah_work_json(ah_engine *e, const char *sid, const char *id, char **json, ah_error *error) {
    if (json)
        *json = NULL;
    ah_code code = core_guard(e, error);
    if (code != AH_OK)
        return code;
    if (!sid || !id || !json)
        return ah_fail(error, AH_INVALID, "session, operation and JSON output required");
    core_operation operation;
    code = core_get_operation(e, sid, id, &operation, error);
    if (code != AH_OK)
        return code;
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = doc ? core_work_value(doc, &operation) : NULL;
    core_operation_free(&operation);
    if (!root) {
        ah_mut_doc_free(doc);
        return ah_fail(error, AH_IO, "cannot encode work snapshot");
    }
    yyjson_mut_doc_set_root(doc, root);
    core_work work;
    code = core_work_load(e, sid, id, &work, error);
    if (code == AH_OK) {
        yyjson_mut_obj_add_uint(doc, root, "version", work.version);
        yyjson_mut_obj_add_uint(doc, root, "generation", work.generation);
        yyjson_mut_obj_add_uint(doc, root, "restarts", work.restarts);
        yyjson_mut_obj_add_uint(doc, root, "wake_at_ms", work.wake_at_ms);
        const char *fields[] = {"checkpoint", "wait", "wake"};
        const char *values[] = {work.checkpoint_json, work.wait_json, work.wake_json};
        for (size_t index = 0; index < 3; index++) {
            yyjson_doc *parsed =
                values[index] ? yyjson_read(values[index], strlen(values[index]), 0) : NULL;
            if (parsed)
                yyjson_mut_obj_add_val(doc, root, fields[index],
                                       yyjson_val_mut_copy(doc, yyjson_doc_get_root(parsed)));
            else
                yyjson_mut_obj_add_null(doc, root, fields[index]);
            yyjson_doc_free(parsed);
        }
        core_work_free(&work);
    } else if (code == AH_NOT_FOUND) {
        code = AH_OK;
        ah_error_clear(error);
    }
    if (code == AH_OK)
        code = add_children(e, sid, id, doc, root, error);
    if (code == AH_OK) {
        *json = ah_mut_write(doc);
        if (!*json)
            code = ah_fail(error, AH_IO, "out of memory encoding work snapshot");
    }
    ah_mut_doc_free(doc);
    return code;
}
