#include "work.h"

enum { SIGNAL_RESERVED = -1, SIGNAL_DELIVERED = 0, SIGNAL_CONSUMED = 1, SIGNAL_EXPIRED = 2 };

static ah_code read_signal(ah_engine *e, const char *id, const char *key, char **json,
                           int *consumed, ah_error *err) {
    *json = NULL;
    *consumed = SIGNAL_DELIVERED;
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(e,
                              "SELECT json,consumed FROM work_signals WHERE operation_id=? "
                              "AND key=?",
                              &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, key, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(s);
    if (step == SQLITE_ROW) {
        *json = ah_strdup((const char *)sqlite3_column_text(s, 0));
        *consumed = sqlite3_column_int(s, 1);
        if (!*json)
            rc = ah_fail(err, AH_IO, "out of memory");
    } else if (step != SQLITE_DONE)
        rc = ah_fail(err, AH_IO, "cannot read durable signal");
    sqlite3_finalize(s);
    return rc;
}

ah_code core_work_wake(ah_engine *e, const char *sid, const char *id, const core_work *work,
                       bool *ready, char **wake, char **signal_key, ah_error *err) {
    *ready = work->wait_json == NULL;
    if (wake)
        *wake = NULL;
    if (signal_key)
        *signal_key = NULL;
    if (!work->wait_json)
        return AH_OK;
    yyjson_doc *doc = yyjson_read(work->wait_json, strlen(work->wait_json), 0);
    yyjson_val *wait = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!yyjson_is_obj(wait)) {
        yyjson_doc_free(doc);
        return ah_fail(err, AH_PROTOCOL, "invalid stored durable wait");
    }
    const char *key = yyjson_get_str(yyjson_obj_get(wait, "signal"));
    char *signal = NULL;
    yyjson_mut_doc *out = wake ? ah_mut_doc_new() : NULL;
    yyjson_mut_val *children = NULL;
    int consumed = SIGNAL_DELIVERED;
    bool child_ready = false;
    ah_code rc = key ? read_signal(e, id, key, &signal, &consumed, err) : AH_OK;
    if (consumed == SIGNAL_RESERVED) {
        free(signal);
        signal = NULL;
    }
    if (rc == AH_OK && consumed >= SIGNAL_CONSUMED)
        rc = ah_fail(err, AH_INVALID, "durable signal key was already consumed");
    if (rc == AH_OK && wake && !out)
        rc = ah_fail(err, AH_IO, "out of memory");
    if (rc == AH_OK)
        rc = core_work_children_ready(e, sid, id, wait, &child_ready, out, &children, err);
    bool timer = work->wake_at_ms && work->wake_at_ms <= e->work_clock_ms();
    *ready = rc == AH_OK && (signal || child_ready || timer);
    if (*ready && wake) {
        yyjson_mut_val *root = yyjson_mut_obj(out);
        yyjson_mut_doc_set_root(out, root);
        yyjson_mut_obj_add_str(out, root, "reason",
                               signal        ? "signal"
                               : child_ready ? "calls"
                                             : "timer");
        if (signal) {
            yyjson_doc *value = yyjson_read(signal, strlen(signal), 0);
            yyjson_mut_val *received = yyjson_mut_obj(out);
            yyjson_mut_obj_add_strcpy(out, received, "key", key);
            yyjson_mut_obj_add_val(out, received, "value",
                                   yyjson_val_mut_copy(out, yyjson_doc_get_root(value)));
            yyjson_mut_obj_add_val(out, root, "signal", received);
            yyjson_doc_free(value);
            *signal_key = ah_strdup(key);
            if (!*signal_key)
                rc = ah_fail(err, AH_IO, "out of memory");
        }
        if (children)
            yyjson_mut_obj_add_val(out, root, "calls", children);
        if (timer)
            yyjson_mut_obj_add_uint(out, root, "until_ms", work->wake_at_ms);
        if (rc == AH_OK)
            *wake = ah_mut_write(out);
        if (rc == AH_OK && !*wake)
            rc = ah_fail(err, AH_IO, "cannot encode durable wake");
    }
    ah_mut_doc_free(out);
    free(signal);
    yyjson_doc_free(doc);
    if (rc != AH_OK) {
        if (wake) {
            free(*wake);
            *wake = NULL;
        }
        if (signal_key) {
            free(*signal_key);
            *signal_key = NULL;
        }
        *ready = false;
    }
    return rc;
}

ah_code core_work_ready(ah_engine *e, const char *sid, const char *id, bool *ready, ah_error *err) {
    *ready = false;
    sqlite3_stmt *row = NULL;
    ah_code rc = core_prepare(
        e, "SELECT wait_json,wake_at_ms FROM operation_work WHERE session_id=? AND operation_id=?",
        &row, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(row, 1, sid, -1, SQLITE_STATIC);
    sqlite3_bind_text(row, 2, id, -1, SQLITE_STATIC);
    int step = sqlite3_step(row);
    if (step == SQLITE_ROW) {
        core_work work = {.wait_json = (char *)sqlite3_column_text(row, 0),
                          .wake_at_ms = (uint64_t)sqlite3_column_int64(row, 1)};
        rc = core_work_wake(e, sid, id, &work, ready, NULL, NULL, err);
    } else
        rc =
            step == SQLITE_DONE ? AH_NOT_FOUND : ah_fail(err, AH_IO, "cannot inspect durable wait");
    sqlite3_finalize(row);
    return rc;
}

static ah_code signal_status(ah_engine *e, const char *sid, const char *id, const char *key,
                             ah_signal_result *result, ah_error *err) {
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(e,
                              "SELECT o.status,w.wait_json,w.wake_at_ms FROM operations o "
                              "JOIN operation_work w ON w.operation_id=o.id "
                              "WHERE o.session_id=? AND o.id=?",
                              &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, id, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(s);
    *result = AH_SIGNAL_NOT_FOUND;
    if (step == SQLITE_ROW) {
        const char *status = (const char *)sqlite3_column_text(s, 0),
                   *wait = (const char *)sqlite3_column_text(s, 1);
        uint64_t until = (uint64_t)sqlite3_column_int64(s, 2);
        bool open = !strcmp(status, "pending") || !strcmp(status, "approved") ||
                    !strcmp(status, "approval") || !strcmp(status, "executing") ||
                    !strcmp(status, "parked") || !strcmp(status, "work_ready");
        if (open && wait && until && until <= e->work_clock_ms()) {
            yyjson_doc *doc = yyjson_read(wait, strlen(wait), 0);
            const char *wanted = yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "signal"));
            if (wanted && !strcmp(wanted, key))
                open = false;
            yyjson_doc_free(doc);
        }
        *result = open ? AH_SIGNAL_DELIVERED : AH_SIGNAL_CLOSED;
    } else if (step != SQLITE_DONE)
        rc = ah_fail(err, AH_IO, "cannot inspect signal destination");
    sqlite3_finalize(s);
    return rc;
}

static ah_code signal_capacity(ah_engine *e, const char *id, ah_error *err) {
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(e,
                              "SELECT count(*) FROM (SELECT 1 FROM work_signals "
                              "WHERE operation_id=? LIMIT ?)",
                              &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(s, 2, CORE_WORK_KEYS_LIMIT);
        if (sqlite3_step(s) != SQLITE_ROW)
            rc = ah_fail(err, AH_IO, "cannot count durable signals");
        else if (sqlite3_column_int(s, 0) >= CORE_WORK_KEYS_LIMIT)
            rc = ah_fail(err, AH_LIMIT, "durable signal limit exceeded");
    }
    sqlite3_finalize(s);
    return rc;
}

ah_code core_work_reserve_signal(ah_engine *e, const char *id, yyjson_val *wait, ah_error *err) {
    const char *key = yyjson_get_str(yyjson_obj_get(wait, "signal"));
    if (!key)
        return AH_OK;
    char *existing = NULL;
    int consumed = SIGNAL_DELIVERED;
    ah_code rc = read_signal(e, id, key, &existing, &consumed, err);
    bool found = existing != NULL;
    free(existing);
    if (rc != AH_OK)
        return rc;
    if (found)
        return consumed >= SIGNAL_CONSUMED
                   ? ah_fail(err, AH_INVALID, "durable signal key was already consumed")
                   : AH_OK;
    rc = signal_capacity(e, id, err);
    sqlite3_stmt *s = NULL;
    if (rc == AH_OK)
        rc = core_prepare(e,
                          "INSERT INTO work_signals(operation_id,key,json,consumed) "
                          "VALUES(?,?,'null',-1)",
                          &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, key, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
    }
    return rc;
}

ah_code core_work_close_signal(ah_engine *e, const char *id, const core_work *work,
                               const char *delivered, ah_error *err) {
    if (!work->wait_json)
        return AH_OK;
    yyjson_doc *doc = yyjson_read(work->wait_json, strlen(work->wait_json), 0);
    const char *key = yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "signal"));
    ah_code rc = AH_OK;
    sqlite3_stmt *s = NULL;
    if (key && rc == AH_OK)
        rc = core_prepare(e,
                          delivered ? "UPDATE work_signals SET consumed=1 WHERE operation_id=? "
                                      "AND key=? AND consumed=0"
                                    : "UPDATE work_signals SET consumed=2 WHERE operation_id=? "
                                      "AND key=? AND consumed=-1",
                          &s, err);
    if (key && rc == AH_OK) {
        sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, key, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
        if (rc == AH_OK && sqlite3_changes(e->db) != 1)
            rc = ah_fail(err, AH_BUSY, "durable signal claim is stale");
    }
    yyjson_doc_free(doc);
    return rc;
}

static ah_code insert_signal(ah_engine *e, const char *sid, const char *id, const char *key,
                             const char *json, ah_signal_result *result, ah_error *err) {
    ah_code rc = signal_status(e, sid, id, key, result, err);
    if (rc != AH_OK || *result != AH_SIGNAL_DELIVERED)
        return rc;
    char *existing = NULL;
    int consumed = SIGNAL_DELIVERED;
    rc = read_signal(e, id, key, &existing, &consumed, err);
    bool found = existing != NULL;
    bool reserved = found && consumed == SIGNAL_RESERVED;
    free(existing);
    if (found && !reserved) {
        *result = consumed == SIGNAL_EXPIRED ? AH_SIGNAL_CLOSED : AH_SIGNAL_DUPLICATE;
        return rc;
    }
    if (rc == AH_OK && !reserved)
        rc = signal_capacity(e, id, err);
    sqlite3_stmt *s = NULL;
    if (rc == AH_OK)
        rc = core_prepare(e,
                          reserved ? "UPDATE work_signals SET json=?3,consumed=0 "
                                     "WHERE operation_id=?1 AND key=?2 AND consumed=-1"
                                   : "INSERT INTO work_signals(operation_id,key,json) "
                                     "VALUES(?,?,?)",
                          &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, json, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
    }
    /* The current wait owns a reserved slot. Publish readiness in the same
     * transaction as delivery, so idle leaf waits need no polling queries. */
    if (rc == AH_OK && reserved)
        rc = core_prepare(e,
                          "UPDATE operations SET status='work_ready' "
                          "WHERE id=? AND session_id=? AND status='parked'",
                          &s, err);
    if (rc == AH_OK && reserved) {
        sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, sid, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
    }
    char *event = rc == AH_OK ? core_object_string("key", key) : NULL;
    if (rc == AH_OK && !event)
        rc = ah_fail(err, AH_IO, "out of memory");
    if (rc == AH_OK)
        rc = core_event(e, sid, id, "work.signaled", event, err);
    free(event);
    return rc;
}

ah_code ah_signal(ah_engine *e, const char *sid, const char *id, const char *key, const char *json,
                  ah_signal_result *result, ah_error *err) {
    if (result)
        *result = AH_SIGNAL_NOT_FOUND;
    ah_code rc = core_guard(e, err);
    if (rc != AH_OK)
        return rc;
    size_t complete = 0;
    if (!sid || !*sid || !id || !*id || !key || !*key || strlen(key) > 128 || !json ||
        !ah_utf8_prefix(key, strlen(key), &complete) || complete != strlen(key) ||
        strlen(json) > CORE_WORK_SIGNAL_LIMIT || !ah_json_valid(json) || !result)
        return ah_fail(err, AH_INVALID, "signal requires bounded key, JSON, and result");
    uint64_t after = 0;
    rc = core_begin(e, &after, err);
    if (rc == AH_OK)
        rc = insert_signal(e, sid, id, key, json, result, err);
    if (rc == AH_OK)
        rc = core_commit(e, after, err);
    else
        core_rollback(e);
    return rc;
}
