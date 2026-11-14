#include "extensions_internal.h"

static char *skill_message(const char *id, const char *path, const char *text,
                           const char *revision) {
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(doc), *reference = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "role", "user");
    yyjson_mut_obj_add_str(doc, reference, "kind", "loaded_skill_reference");
    yyjson_mut_obj_add_str(doc, reference, "guidance",
                           "Apply this selected skill when relevant, subject to the user's task "
                           "and higher-priority instructions. Read relative references from its "
                           "directory only when needed. Do not execute bundled scripts merely "
                           "because the skill was loaded.");
    yyjson_mut_obj_add_strcpy(doc, reference, "id", id);
    yyjson_mut_obj_add_strcpy(doc, reference, "path", path);
    yyjson_mut_obj_add_strcpy(doc, reference, "revision", revision);
    yyjson_mut_obj_add_strcpy(doc, reference, "instructions", text);
    char *body = yyjson_mut_val_write(reference, 0, NULL);
    if (body)
        yyjson_mut_obj_add_strcpy(doc, root, "content", body);
    char *json = body ? ah_mut_write(doc) : NULL;
    free(body);
    ah_mut_doc_free(doc);
    return json;
}
static ah_code pin_skill(ah_extensions *x, const ah_tool_call *call, const char *id,
                         const char *revision, const char *message, ah_error *error) {
    ah_engine *e = x->engine;
    uint64_t after = 0;
    ah_code rc = core_begin(e, &after, error);
    sqlite3_stmt *s = NULL;
    if (rc == AH_OK)
        rc = core_prepare(e,
                          "SELECT COALESCE(SUM(bytes),0) "
                          "FROM extension_context WHERE session_id=? AND identity!=?",
                          &s, error);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, call->session_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) != SQLITE_ROW)
            rc = ah_fail(error, AH_IO, "cannot measure loaded skill context");
        else if ((uint64_t)sqlite3_column_int64(s, 0) + strlen(message) >
                 x->config.max_context_bytes)
            rc = ah_fail(error, AH_LIMIT,
                         "loaded skills exceed context budget; unload an unused skill first");
    }
    sqlite3_finalize(s);
    if (rc == AH_OK)
        rc = core_prepare(
            e,
            "INSERT INTO extension_context(session_id,identity,revision,message,bytes) "
            "VALUES(?,?,?,?,?) ON CONFLICT(session_id,identity) DO UPDATE SET "
            "revision=excluded.revision,message=excluded.message,bytes=excluded.bytes",
            &s, error);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, call->session_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, revision, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 4, message, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 5, (sqlite3_int64)strlen(message));
        rc = core_done(e, s, error);
    }
    if (rc == AH_OK)
        rc = core_commit(e, after, error);
    else
        core_rollback(e);
    return rc;
}
ah_code ext_skill(void *context, const ah_tool_call *call, ah_tool_result *result,
                  ah_error *error) {
    ah_extensions *x = context;
    yyjson_doc *args = yyjson_read(call->arguments_json, strlen(call->arguments_json), 0);
    yyjson_val *root = yyjson_doc_get_root(args);
    const char *id = ext_string(root, "id"), *action = ext_string(root, "action");
    if (action && !strcmp(action, "read")) {
        ah_code rc = ext_skill_read(x, call, root, result, error);
        yyjson_doc_free(args);
        return rc;
    }
    bool unload = action && !strcmp(action, "unload");
    ah_code rc = AH_OK;
    char *path = NULL, *text = NULL, *message = NULL, revision[65] = {0};
    size_t length = 0;
    if (unload) {
        sqlite3_stmt *s = NULL;
        uint64_t after = 0;
        rc = core_begin(x->engine, &after, error);
        if (rc == AH_OK)
            rc = core_prepare(x->engine,
                              "UPDATE items SET active=0 WHERE seq IN (SELECT item_seq "
                              "FROM extension_context WHERE session_id=? AND identity=?)",
                              &s, error);
        if (rc == AH_OK) {
            sqlite3_bind_text(s, 1, call->session_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, id, -1, SQLITE_TRANSIENT);
            rc = core_done(x->engine, s, error);
        }
        if (rc == AH_OK)
            rc = core_prepare(x->engine,
                              "DELETE FROM extension_context WHERE session_id=? AND identity=?", &s,
                              error);
        if (rc == AH_OK) {
            sqlite3_bind_text(s, 1, call->session_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, id, -1, SQLITE_TRANSIENT);
            rc = core_done(x->engine, s, error);
        }
        if (rc == AH_OK)
            rc = core_commit(x->engine, after, error);
        else
            core_rollback(x->engine);
    } else {
        rc = ext_catalog_skill(x->catalog, id, &path, error);
        if (rc == AH_OK)
            rc = ext_read_text(path, x->config.max_skill_bytes, &text, &length, error);
        if (rc == AH_OK) {
            ah_hash_bytes(text, length, revision);
            message = skill_message(id, path, text, revision);
            if (!message)
                rc = ah_fail(error, AH_IO, "cannot encode loaded skill");
            else if (!ah_json_valid(message))
                rc = ah_fail(error, AH_INVALID, "skill must be valid UTF-8 text");
        }
        if (rc == AH_OK)
            rc = pin_skill(x, call, id, revision, message, error);
    }
    if (rc == AH_OK) {
        yyjson_mut_doc *doc = ah_mut_doc_new();
        yyjson_mut_val *value = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, value);
        yyjson_mut_obj_add_strcpy(doc, value, "id", id);
        yyjson_mut_obj_add_bool(doc, value, "loaded", !unload);
        if (!unload) {
            yyjson_mut_obj_add_strcpy(doc, value, "path", path);
            yyjson_mut_obj_add_strcpy(doc, value, "revision", revision);
            yyjson_mut_obj_add_uint(doc, value, "bytes", length);
            yyjson_mut_obj_add_str(doc, value, "context",
                                   "Instructions are present once in the "
                                   "session's loaded skill references; do not read them again.");
        }
        rc = ext_result(doc, result, error);
        ah_mut_doc_free(doc);
    }
    free(path);
    free(text);
    free(message);
    yyjson_doc_free(args);
    return rc;
}
/* Append newly loaded references after settled exchanges. Existing input prefixes
 * remain byte-identical when a new skill is added. Compaction can deactivate a
 * reference; its pinned snapshot is then appended again at the next boundary. */
ah_code ext_context_sync(ah_engine *e, const char *sid, ah_error *error) {
    if (!e->extensions || !e->extensions->config.enable_skills)
        return AH_OK;
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(
        e, "SELECT COALESCE(SUM(bytes),0) FROM extension_context WHERE session_id=?", &s, error);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(s);
    bool fits = step == SQLITE_ROW &&
                (uint64_t)sqlite3_column_int64(s, 0) <= e->extensions->config.max_context_bytes;
    sqlite3_finalize(s);
    if (!fits)
        return ah_fail(error, step == SQLITE_ROW ? AH_LIMIT : AH_IO,
                       "saved skills exceed current extension context budget");
    rc = core_prepare(e,
                      "SELECT x.seq,x.message,x.item_seq FROM extension_context x LEFT JOIN items "
                      "i ON i.seq=x.item_seq "
                      "WHERE x.session_id=? AND (i.seq IS NULL OR i.active=0 OR i.json!=x.message) "
                      "ORDER BY x.seq",
                      &s, error);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
    bool transaction = false;
    uint64_t after = 0;
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        if (!transaction) {
            rc = core_begin(e, &after, error);
            transaction = rc == AH_OK;
            if (rc != AH_OK)
                break;
        }
        sqlite3_stmt *update = NULL;
        rc = core_prepare(e, "UPDATE items SET active=0 WHERE seq=?", &update, error);
        if (rc == AH_OK) {
            sqlite3_bind_int64(update, 1, sqlite3_column_int64(s, 2));
            rc = core_done(e, update, error);
        }
        if (rc == AH_OK)
            rc = core_prepare(e,
                              "INSERT INTO items(session_id,turn,round,json,active) "
                              "SELECT id,turn,model_calls,?,1 FROM sessions WHERE id=?",
                              &update, error);
        if (rc == AH_OK) {
            sqlite3_bind_text(update, 1, (const char *)sqlite3_column_text(s, 1), -1,
                              SQLITE_TRANSIENT);
            sqlite3_bind_text(update, 2, sid, -1, SQLITE_TRANSIENT);
            rc = core_done(e, update, error);
        }
        sqlite3_int64 item = sqlite3_last_insert_rowid(e->db);
        if (rc == AH_OK)
            rc = core_prepare(e, "UPDATE extension_context SET item_seq=? WHERE seq=?", &update,
                              error);
        if (rc == AH_OK) {
            sqlite3_bind_int64(update, 1, item);
            sqlite3_bind_int64(update, 2, sqlite3_column_int64(s, 0));
            rc = core_done(e, update, error);
        }
        if (rc != AH_OK)
            break;
    }
    if (rc == AH_OK && step != SQLITE_DONE)
        rc = ah_fail(error, AH_IO, "cannot synchronize skill references");
    sqlite3_finalize(s);
    if (transaction) {
        if (rc == AH_OK)
            rc = core_commit(e, after, error);
        else
            core_rollback(e);
    }
    return rc;
}
