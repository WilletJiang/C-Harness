#include "work.h"

ah_code core_work_schema(ah_engine *e, ah_error *err) {
    ah_code rc = core_sql(
        e,
        "CREATE TABLE IF NOT EXISTS operation_work("
        "operation_id TEXT PRIMARY KEY REFERENCES operations(id) ON DELETE CASCADE,"
        "session_id TEXT NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
        "version INTEGER NOT NULL CHECK(version>0),replay INTEGER NOT NULL,"
        "max_restarts INTEGER NOT NULL CHECK(max_restarts>0),restarts INTEGER NOT NULL DEFAULT 0,"
        "generation INTEGER NOT NULL DEFAULT 0,checkpoint_json TEXT,wait_json TEXT,wake_json TEXT,"
        "wake_at_ms INTEGER NOT NULL DEFAULT 0);"
        "CREATE INDEX IF NOT EXISTS work_wake ON operation_work(session_id,wake_at_ms) "
        "WHERE wake_at_ms>0;"
        "CREATE TABLE IF NOT EXISTS work_signals("
        "operation_id TEXT NOT NULL REFERENCES operations(id) ON DELETE CASCADE,"
        "key TEXT NOT NULL,json TEXT NOT NULL,consumed INTEGER NOT NULL DEFAULT 0 "
        "CHECK(consumed IN(-1,0,1,2)),"
        "PRIMARY KEY(operation_id,key));",
        err);
    return rc == AH_OK ? core_work_calls_schema(e, err) : rc;
}

ah_code core_work_seed(ah_engine *e, const char *sid, const core_operation *op,
                       const core_tool *tool, ah_error *err) {
    if (!tool || !tool->tool.work)
        return AH_OK;
    e->has_work = true;
    const ah_work_config *config = tool->tool.work;
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(e,
                              "INSERT INTO operation_work(operation_id,session_id,version,replay,"
                              "max_restarts) VALUES(?,?,?,?,?)",
                              &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, op->id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, sid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 3, config->version);
    sqlite3_bind_int(s, 4, config->replay);
    sqlite3_bind_int64(s, 5, config->max_restarts ? config->max_restarts : 5);
    return core_done(e, s, err);
}

void core_work_free(core_work *work) {
    free(work->checkpoint_json);
    free(work->wait_json);
    free(work->wake_json);
    memset(work, 0, sizeof(*work));
}

ah_code core_work_load(ah_engine *e, const char *sid, const char *id, core_work *work,
                       ah_error *err) {
    memset(work, 0, sizeof(*work));
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(e,
                              "SELECT version,replay,max_restarts,restarts,generation,"
                              "checkpoint_json,wait_json,wake_json,wake_at_ms "
                              "FROM operation_work WHERE operation_id=? AND session_id=?",
                              &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, sid, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(s);
    if (step != SQLITE_ROW) {
        sqlite3_finalize(s);
        return step == SQLITE_DONE ? AH_NOT_FOUND : ah_fail(err, AH_IO, "cannot read durable work");
    }
    work->version = (unsigned)sqlite3_column_int64(s, 0);
    work->replay = (ah_replay)sqlite3_column_int(s, 1);
    work->max_restarts = (unsigned)sqlite3_column_int64(s, 2);
    work->restarts = (unsigned)sqlite3_column_int64(s, 3);
    work->generation = (uint64_t)sqlite3_column_int64(s, 4);
    char **strings[] = {&work->checkpoint_json, &work->wait_json, &work->wake_json};
    for (int i = 0; i < 3; i++) {
        if (sqlite3_column_type(s, i + 5) == SQLITE_NULL)
            continue;
        *strings[i] = ah_strdup((const char *)sqlite3_column_text(s, i + 5));
        if (!*strings[i])
            rc = ah_fail(err, AH_IO, "out of memory");
    }
    work->wake_at_ms = (uint64_t)sqlite3_column_int64(s, 8);
    sqlite3_finalize(s);
    if (rc != AH_OK)
        core_work_free(work);
    return rc;
}

/* Builtin flow waits for every child in a wave. Once done, those receipts
 * are immutable. Store the compact wait object as its wake reference. Public
 * wake snapshots always start with "reason" and are still read verbatim. */
static bool flow_wait(const core_operation *op, const core_work *work) {
    if (strcmp(op->name, "flow") || !work->wait_json)
        return false;
    yyjson_doc *doc = yyjson_read(work->wait_json, strlen(work->wait_json), 0);
    yyjson_val *wait = yyjson_doc_get_root(doc);
    bool all = yyjson_obj_size(wait) == 1 && yyjson_is_arr(yyjson_obj_get(wait, "calls"));
    yyjson_doc_free(doc);
    return all;
}

ah_code core_work_materialize_wake(ah_engine *e, const char *sid, const char *id, core_work *work,
                                   ah_error *error) {
    if (!work->wake_json || strncmp(work->wake_json, "{\"calls\":[", sizeof("{\"calls\":[") - 1))
        return AH_OK;
    core_work wait = {.wait_json = work->wake_json};
    char *wake = NULL, *signal = NULL;
    bool ready = false;
    ah_code code = core_work_wake(e, sid, id, &wait, &ready, &wake, &signal, error);
    free(signal);
    if (code == AH_OK && !ready)
        code = ah_fail(error, AH_PROTOCOL, "stored flow wake references unsettled children");
    if (code == AH_OK) {
        free(work->wake_json);
        work->wake_json = wake;
    } else
        free(wake);
    return code;
}

static bool failed_join(const core_work *work, const char *wake) {
    if (!work->wait_json || !wake)
        return false;
    yyjson_doc *waiting = yyjson_read(work->wait_json, strlen(work->wait_json), 0),
               *received = NULL;
    const char *policy = yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(waiting), "policy"));
    bool failed = false;
    if (policy && !strcmp(policy, "fail_fast")) {
        received = yyjson_read(wake, strlen(wake), 0);
        yyjson_val *calls = yyjson_obj_get(yyjson_doc_get_root(received), "calls"), *key, *value;
        size_t index, count;
        yyjson_obj_foreach(calls, index, count, key, value) {
            const char *status = yyjson_get_str(yyjson_obj_get(value, "status"));
            if (status && !strcmp(status, "done") &&
                yyjson_get_bool(yyjson_obj_get(value, "is_error")))
                failed = true;
        }
    }
    yyjson_doc_free(waiting);
    yyjson_doc_free(received);
    return failed;
}

ah_code core_work_claim(ah_engine *e, const char *sid, core_operation *op, ah_error *err) {
    const char *id = op->id;
    core_work work;
    ah_code rc = core_work_load(e, sid, id, &work, err);
    if (rc != AH_OK)
        return rc;
    bool reference = flow_wait(op, &work);
    bool ready = false;
    char *wake = NULL, *signal = NULL;
    rc = core_work_wake(e, sid, id, &work, &ready, reference ? NULL : &wake, &signal, err);
    if (rc == AH_OK && !ready)
        rc = ah_fail(err, AH_BUSY, "durable work is not ready");
    if (rc == AH_OK && failed_join(&work, wake)) {
        rc = core_work_finish_children(e, sid, id, err);
        if (rc == AH_OK) {
            free(wake);
            free(signal);
            rc = core_work_wake(e, sid, id, &work, &ready, &wake, &signal, err);
        }
    }
    if (rc == AH_OK && work.generation >= INT64_MAX)
        rc = ah_fail(err, AH_LIMIT, "durable generation exhausted");
    sqlite3_stmt *s = NULL;
    if (rc == AH_OK)
        rc = core_prepare(e,
                          "UPDATE operation_work SET generation=generation+1,wait_json=NULL,"
                          "wake_at_ms=0,wake_json=? WHERE operation_id=? AND session_id=? "
                          "AND generation=? AND EXISTS(SELECT 1 FROM operations WHERE id=?2 "
                          "AND status IN('pending','approved','parked','work_ready'))",
                          &s, err);
    if (rc == AH_OK) {
        const char *value = reference ? work.wait_json : (wake ? wake : work.wake_json);
        if (value)
            sqlite3_bind_text(s, 1, value, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, sid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 4, (sqlite3_int64)work.generation);
        rc = core_done(e, s, err);
        if (rc == AH_OK && sqlite3_changes(e->db) != 1)
            rc = ah_fail(err, AH_BUSY, "durable work claim is stale");
    }
    if (rc == AH_OK)
        rc = core_work_close_signal(e, id, &work, signal, err);
    if (rc == AH_OK)
        rc = core_set_operation(e, sid, id, "executing", err);
    free(wake);
    free(signal);
    if (rc == AH_OK)
        op->generation = work.generation + 1;
    core_work_free(&work);
    return rc;
}

static ah_code continuation_wait(yyjson_val *wait, uint64_t *until, ah_error *err) {
    if (!wait)
        return AH_OK;
    yyjson_val *signal = yyjson_obj_get(wait, "signal"), *timer = yyjson_obj_get(wait, "until_ms"),
               *calls = yyjson_obj_get(wait, "calls"), *policy = yyjson_obj_get(wait, "policy");
    if (!yyjson_is_obj(wait) || (!signal && !timer && !calls) ||
        (signal &&
         (!yyjson_is_str(signal) || !yyjson_get_len(signal) || yyjson_get_len(signal) > 128)) ||
        (timer && (!yyjson_is_uint(timer) || !yyjson_get_uint(timer) ||
                   yyjson_get_uint(timer) > INT64_MAX)) ||
        (calls && (!yyjson_is_arr(calls) || !yyjson_arr_size(calls))) ||
        (policy && (!calls || !yyjson_is_str(policy))))
        return ah_fail(err, AH_INVALID, "invalid durable wait");
    if (policy && strcmp(yyjson_get_str(policy), "all") && strcmp(yyjson_get_str(policy), "any") &&
        strcmp(yyjson_get_str(policy), "fail_fast"))
        return ah_fail(err, AH_INVALID, "invalid durable wait policy");
    *until = yyjson_get_uint(timer);
    return AH_OK;
}

static ah_code checkpoint_changed(ah_engine *e, const char *sid, const char *id,
                                  yyjson_val *checkpoint, bool *changed, ah_error *err) {
    *changed = false;
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(e,
                              "SELECT checkpoint_json FROM operation_work WHERE operation_id=? "
                              "AND session_id=?",
                              &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, sid, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(s);
    if (step == SQLITE_ROW) {
        const char *previous = (const char *)sqlite3_column_text(s, 0);
        if (!previous)
            previous = "{}";
        yyjson_doc *doc = yyjson_read(previous, strlen(previous), 0);
        if (!doc)
            rc = ah_fail(err, AH_PROTOCOL, "invalid stored durable checkpoint");
        else
            *changed = !yyjson_equals(checkpoint, yyjson_doc_get_root(doc));
        yyjson_doc_free(doc);
    } else
        rc = step == SQLITE_DONE ? AH_NOT_FOUND
                                 : ah_fail(err, AH_IO, "cannot read durable checkpoint");
    sqlite3_finalize(s);
    return rc;
}

ah_code core_work_apply(ah_engine *e, const char *sid, const char *id, uint64_t generation,
                        const char *json, ah_error *err) {
    if (!json || strlen(json) > CORE_WORK_JSON_LIMIT || !ah_json_valid(json) ||
        generation > INT64_MAX)
        return ah_fail(err, AH_INVALID, "bounded valid continuation required");
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL,
               *checkpoint = yyjson_obj_get(root, "checkpoint"),
               *wait = yyjson_obj_get(root, "wait"), *calls = yyjson_obj_get(root, "calls");
    uint64_t until = 0;
    ah_code rc = yyjson_is_obj(root) && yyjson_is_obj(checkpoint)
                     ? continuation_wait(wait, &until, err)
                     : ah_fail(err, AH_INVALID, "continuation requires an object checkpoint");
    char *saved = rc == AH_OK ? ah_json_write(checkpoint) : NULL;
    char *waiting = rc == AH_OK && wait ? ah_json_write(wait) : NULL;
    if (rc == AH_OK && (!saved || (wait && !waiting)))
        rc = ah_fail(err, AH_IO, "out of memory");
    bool changed = false;
    if (rc == AH_OK)
        rc = checkpoint_changed(e, sid, id, checkpoint, &changed, err);
    sqlite3_stmt *s = NULL;
    if (rc == AH_OK)
        rc = core_prepare(e,
                          "UPDATE operation_work SET checkpoint_json=?1,wait_json=?2,"
                          "wake_json=NULL,"
                          "wake_at_ms=?3,restarts=CASE WHEN ?7 THEN 0 ELSE restarts END "
                          "WHERE operation_id=?4 AND session_id=?5 "
                          "AND generation=?6 AND EXISTS(SELECT 1 FROM operations WHERE id=?4 "
                          "AND status='executing')",
                          &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, saved, -1, SQLITE_TRANSIENT);
        if (waiting)
            sqlite3_bind_text(s, 2, waiting, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 3, (sqlite3_int64)until);
        sqlite3_bind_text(s, 4, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 5, sid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 6, (sqlite3_int64)generation);
        sqlite3_bind_int(s, 7, changed);
        rc = core_done(e, s, err);
        if (rc == AH_OK && sqlite3_changes(e->db) != 1)
            rc = ah_fail(err, AH_BUSY, "durable continuation is stale");
    }
    if (rc == AH_OK && calls)
        rc = core_work_children(e, sid, id, generation, calls, err);
    if (rc == AH_OK && wait)
        rc = core_work_reserve_signal(e, id, wait, err);
    bool ready = !wait;
    if (rc == AH_OK && wait) {
        core_work pending = {.wait_json = waiting, .wake_at_ms = until};
        rc = core_work_wake(e, sid, id, &pending, &ready, NULL, NULL, err);
    }
    if (rc == AH_OK)
        rc = core_set_operation(e, sid, id, ready ? "work_ready" : "parked", err);
    if (rc == AH_OK)
        rc = core_event(e, sid, id, wait ? "work.parked" : "work.checkpoint", json, err);
    free(saved);
    free(waiting);
    yyjson_doc_free(doc);
    return rc;
}

ah_code core_work_recover(ah_engine *e, const char *sid, const char *id, bool *handled,
                          bool *unknown, ah_error *err) {
    *handled = false;
    *unknown = false;
    core_work work;
    ah_code rc = core_work_load(e, sid, id, &work, err);
    if (rc == AH_NOT_FOUND)
        return AH_OK;
    if (rc != AH_OK)
        return rc;
    bool replay = work.replay == AH_REPLAY_SAFE && work.restarts < work.max_restarts;
    if (!replay) {
        core_work_free(&work);
        return AH_OK;
    }
    *handled = true;
    sqlite3_stmt *s = NULL;
    rc = core_prepare(e,
                      "UPDATE operation_work SET restarts=restarts+1,generation=generation+1 "
                      "WHERE operation_id=? AND session_id=? AND generation<?",
                      &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, sid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 3, INT64_MAX);
        rc = core_done(e, s, err);
        if (rc == AH_OK && sqlite3_changes(e->db) != 1)
            rc = ah_fail(err, AH_LIMIT, "durable generation exhausted");
    }
    if (rc == AH_OK)
        rc = core_set_operation(e, sid, id, "work_ready", err);
    if (rc == AH_OK)
        rc = core_event(e, sid, id, "work.recovered", "{}", err);
    core_work_free(&work);
    return rc;
}

ah_code core_work_next_wake(ah_engine *e, const char *sid, uint64_t *out, ah_error *err) {
    *out = 0;
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(e,
                              "SELECT w.wake_at_ms FROM operation_work w JOIN operations o "
                              "ON o.id=w.operation_id WHERE w.session_id=? AND w.wake_at_ms>0 "
                              "AND o.status='parked' ORDER BY w.wake_at_ms LIMIT 1",
                              &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(s);
    if (step == SQLITE_ROW)
        *out = (uint64_t)sqlite3_column_int64(s, 0);
    else if (step != SQLITE_DONE)
        rc = ah_fail(err, AH_IO, "cannot read next durable wake");
    sqlite3_finalize(s);
    return rc;
}
