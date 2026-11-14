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
        rc = core_prepare(x->engine,
                          "DELETE FROM extension_context WHERE session_id=? AND identity=?", &s,
                          error);
        if (rc == AH_OK) {
            sqlite3_bind_text(s, 1, call->session_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, id, -1, SQLITE_TRANSIENT);
            rc = core_done(x->engine, s, error);
        }
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
ah_code ext_context(ah_engine *e, const char *sid, yyjson_mut_doc *doc, yyjson_mut_val *input,
                    ah_error *error) {
    if (!e->extensions || !e->extensions->config.enable_skills)
        return AH_OK;
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(e,
                              "SELECT message FROM extension_context WHERE session_id=? "
                              "ORDER BY seq",
                              &s, error);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
    size_t bytes = 0;
    int step;
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        const char *json = (const char *)sqlite3_column_text(s, 0);
        bytes += strlen(json);
        if (bytes > e->extensions->config.max_context_bytes) {
            rc = ah_fail(error, AH_LIMIT, "saved skills exceed current extension context budget");
            break;
        }
        yyjson_doc *parsed = yyjson_read(json, strlen(json), 0);
        if (!parsed) {
            rc = ah_fail(error, AH_IO, "corrupt saved skill context");
            break;
        }
        yyjson_mut_arr_append(input, yyjson_val_mut_copy(doc, yyjson_doc_get_root(parsed)));
        yyjson_doc_free(parsed);
    }
    if (rc == AH_OK && step != SQLITE_DONE)
        rc = ah_fail(error, AH_IO, "cannot read saved skill context");
    sqlite3_finalize(s);
    return rc;
}
