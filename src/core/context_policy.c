#include "core.h"

static const char recall_schema[] =
    "{\"type\":\"object\",\"properties\":{\"checkpoint\":{\"type\":\"string\"},"
    "\"after_seq\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":9007199254740991},"
    "\"offset\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":9007199254740991},"
    "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":65536}},"
    "\"additionalProperties\":false}";

const char *core_context_tool_schema(void) {
    return recall_schema;
}

ah_code core_context_restore(ah_engine *e, ah_error *error) {
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(e, "SELECT EXISTS(SELECT 1 FROM context_checkpoint_refs)", &s, error);
    if (rc == AH_OK) {
        if (sqlite3_step(s) == SQLITE_ROW)
            e->context_history = sqlite3_column_int(s, 0) != 0;
        else
            rc = ah_fail(error, AH_IO, "cannot restore context checkpoint visibility");
    }
    sqlite3_finalize(s);
    return rc;
}

static ah_code add_column(ah_engine *e, const char *query, const char *ddl, ah_error *error) {
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(e, query, &s, error);
    if (rc != AH_OK)
        return rc;
    int step = sqlite3_step(s);
    bool present = step == SQLITE_ROW && sqlite3_column_int(s, 0) != 0;
    sqlite3_finalize(s);
    if (step != SQLITE_ROW)
        return ah_fail(error, AH_IO, "cannot inspect context schema");
    return present ? AH_OK : core_sql(e, ddl, error);
}
ah_code core_context_schema(ah_engine *e, int version, ah_error *error) {
    (void)version; /* Inspect columns too: migration fixtures can reset schema markers. */
    ah_code rc = add_column(e, "SELECT COUNT(*) FROM pragma_table_info('items') WHERE name='round'",
                            "ALTER TABLE items ADD COLUMN round INTEGER NOT NULL DEFAULT 0", error);
    if (rc == AH_OK)
        rc = add_column(
            e, "SELECT COUNT(*) FROM pragma_table_info('extension_context') WHERE name='item_seq'",
            "ALTER TABLE extension_context ADD COLUMN item_seq INTEGER REFERENCES items(seq)",
            error);
    if (rc == AH_OK)
        rc = core_sql(
            e,
            "CREATE TABLE IF NOT EXISTS context_checkpoints(id TEXT PRIMARY KEY,session_id TEXT "
            "NOT NULL REFERENCES sessions(id),summary TEXT NOT NULL,before_bytes INTEGER NOT NULL,"
            "after_bytes INTEGER NOT NULL);"
            "CREATE TABLE IF NOT EXISTS context_archive_items(checkpoint_id TEXT NOT NULL "
            "REFERENCES context_checkpoints(id),item_seq INTEGER NOT NULL REFERENCES items(seq),"
            "PRIMARY KEY(checkpoint_id,item_seq));"
            "CREATE TABLE IF NOT EXISTS context_checkpoint_refs(session_id TEXT NOT NULL "
            "REFERENCES sessions(id),checkpoint_id TEXT NOT NULL REFERENCES "
            "context_checkpoints(id),"
            "PRIMARY KEY(session_id,checkpoint_id));",
            error);
    return rc;
}
void ah_context_config_init(ah_context_config *config) {
    if (!config)
        return;
    memset(config, 0, sizeof(*config));
    config->keep_recent_exchanges = 3;
}
ah_code ah_context_enable(ah_engine *e, const ah_context_config *config, ah_error *error) {
    ah_code rc = core_guard(e, error);
    if (rc != AH_OK)
        return rc;
    if (!config)
        return ah_fail(error, AH_INVALID, "context configuration required");
    ah_context_config policy = *config;
    size_t maximum = e->config.max_context_bytes;
    if (!policy.reserve_bytes)
        policy.reserve_bytes = maximum / 8 < 65536 ? maximum / 8 : 65536;
    if (!policy.trigger_bytes)
        policy.trigger_bytes = maximum - maximum / 4;
    if (!policy.target_bytes)
        policy.target_bytes = maximum / 2;
    if (policy.target_bytes < 1024 || policy.target_bytes >= policy.trigger_bytes ||
        policy.reserve_bytes >= maximum || policy.trigger_bytes > maximum - policy.reserve_bytes ||
        policy.keep_recent_exchanges > 64)
        return ah_fail(error, AH_INVALID,
                       "context policy requires target < trigger <= limit-reserve");
    if (rc == AH_OK) {
        e->context_policy = policy;
        e->context_enabled = true;
    }
    return rc;
}
ah_code ah_context_json(ah_engine *e, const char *sid, char **json, ah_error *error) {
    if (json)
        *json = NULL;
    ah_code rc = core_guard(e, error);
    if (rc != AH_OK)
        return rc;
    if (!sid || !json)
        return ah_fail(error, AH_INVALID, "session and output required");
    core_session session;
    rc = core_load(e, sid, &session, error);
    if (rc != AH_OK)
        return rc;
    core_session_free(&session);
    sqlite3_stmt *s = NULL;
    rc = core_prepare(
        e,
        "SELECT COUNT(*),COALESCE(SUM(c.before_bytes-c.after_bytes),0) "
        "FROM context_checkpoint_refs r JOIN context_checkpoints c ON c.id=r.checkpoint_id "
        "WHERE r.session_id=?",
        &s, error);
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_bool(doc, root, "enabled", e->context_enabled);
    yyjson_mut_obj_add_uint(doc, root, "trigger_bytes", e->context_policy.trigger_bytes);
    yyjson_mut_obj_add_uint(doc, root, "target_bytes", e->context_policy.target_bytes);
    yyjson_mut_obj_add_uint(doc, root, "reserve_bytes", e->context_policy.reserve_bytes);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) != SQLITE_ROW)
            rc = ah_fail(error, AH_IO, "cannot read context checkpoints");
        else {
            yyjson_mut_obj_add_sint(doc, root, "checkpoints", sqlite3_column_int64(s, 0));
            yyjson_mut_obj_add_sint(doc, root, "archived_request_bytes",
                                    sqlite3_column_int64(s, 1));
        }
    }
    sqlite3_finalize(s);
    if (rc == AH_OK && !(*json = ah_mut_write(doc)))
        rc = ah_fail(error, AH_IO, "out of memory");
    ah_mut_doc_free(doc);
    return rc;
}
ah_code core_context_fork(ah_engine *e, const char *source, const char *target, ah_error *error) {
    sqlite3_stmt *s = NULL;
    ah_code rc =
        core_prepare(e,
                     "INSERT INTO context_checkpoint_refs(session_id,checkpoint_id) "
                     "SELECT ?,checkpoint_id FROM context_checkpoint_refs WHERE session_id=?",
                     &s, error);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, target, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, source, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, error);
    }
    if (rc == AH_OK)
        rc = core_prepare(
            e,
            "UPDATE extension_context SET item_seq=(SELECT MAX(i.seq) FROM items i "
            "WHERE i.session_id=extension_context.session_id AND i.json=extension_context.message) "
            "WHERE session_id=?",
            &s, error);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, target, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, error);
    }
    return rc;
}
