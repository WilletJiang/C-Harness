#include "core.h"
#include "../tools/local_internal.h"

static char *observation_reference(const char *id, yyjson_val *result, bool search) {
    static const char *const page[] = {
        "snapshot",    "next_offset",    "truncated", "scan_truncated",
        "total_files", "total_excerpts", NULL};
    static const char *const file[] = {"path",  "revision", "offset_bytes", "bytes", "start_line",
                                       "lines", NULL};
    yyjson_mut_doc *doc = ah_mut_doc_new();
    if (!doc)
        return NULL;
    yyjson_mut_val *root = yyjson_mut_obj(doc), *ref = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_bool(doc, root, "is_error", false);
    yyjson_mut_obj_add_val(doc, root, "result", ref);
    yyjson_mut_obj_add_bool(doc, ref, "reused", true);
    yyjson_mut_obj_add_str(doc, ref, "retrieval_tool", "read_result");
    yyjson_mut_obj_add_strcpy(doc, ref, "operation_id", id);
    for (const char *const *field = search ? page : file; *field; field++) {
        yyjson_val *value = yyjson_obj_get(result, *field);
        if (value)
            yyjson_mut_obj_add_val(doc, ref, *field, yyjson_val_mut_copy(doc, value));
    }
    char *view = ah_mut_write(doc);
    ah_mut_doc_free(doc);
    return view;
}

/* Only complete model-visible read receipts can justify replacing another read
 * with a reference. The active-item join handles compaction; forks start empty. */
char *core_observation_view(ah_engine *e, const char *sid, const core_operation *op,
                            const char *json, bool is_error, char digest[65], ah_error *error) {
    digest[0] = 0;
    char *view = core_result_view(e, op->id, json, is_error);
    bool file = !strcmp(op->name, "read_file");
    core_tool *tool = !strcmp(op->name, "find") ? core_find_tool(e, op->name) : NULL;
    bool search = tool && tool->tool.execute == ah_local_find;
    bool extension = !strcmp(op->name, "discover") || !strcmp(op->name, "mcp");
    if (!view || is_error || (!file && !search && !extension))
        return view;
    yyjson_doc *args = yyjson_read(op->arguments, strlen(op->arguments), 0),
               *parsed = yyjson_read(view, strlen(view), 0);
    yyjson_val *result = parsed ? yyjson_obj_get(yyjson_doc_get_root(parsed), "result") : NULL;
    yyjson_val *revision = yyjson_obj_get(result, "revision"),
               *content = yyjson_obj_get(result, "content");
    bool eligible =
        args && ((file && yyjson_is_str(content) && yyjson_get_len(revision) == 64) ||
                 (search && yyjson_is_true(yyjson_obj_get(result, "ok")) &&
                  yyjson_is_arr(yyjson_obj_get(result, "results")) &&
                  yyjson_get_len(yyjson_obj_get(result, "snapshot")) == 64) ||
                 (extension && result && !yyjson_is_true(yyjson_obj_get(result, "truncated"))));
    bool force = args && yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(args), "force"));
    if (eligible)
        ah_hash_bytes(json, strlen(json), digest);
    if (eligible && !force) {
        sqlite3_stmt *s = NULL;
        ah_code code = core_prepare(
            e,
            "SELECT o.operation_id FROM observations o JOIN items i ON i.seq=o.item_seq "
            "WHERE o.session_id=? AND o.digest=? AND i.active=1 ORDER BY i.seq LIMIT 1",
            &s, error);
        if (code == AH_OK) {
            sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, digest, -1, SQLITE_TRANSIENT);
            int step = sqlite3_step(s);
            if (step == SQLITE_ROW) {
                const char *id = (const char *)sqlite3_column_text(s, 0);
                char *reused = observation_reference(id, result, search);
                if (reused && strlen(reused) < strlen(view)) {
                    uint64_t now = ah_now_us();
                    core_metric(e, sid, op->id, "read.reused", now, now, strlen(view), 1, AH_OK);
                    free(view);
                    view = reused;
                    digest[0] = 0;
                } else
                    free(reused);
            } else if (step != SQLITE_DONE) {
                free(view);
                view = NULL;
            }
        } else {
            free(view);
            view = NULL;
        }
        sqlite3_finalize(s);
    }
    yyjson_doc_free(args);
    yyjson_doc_free(parsed);
    return view;
}

ah_code core_observation_save(ah_engine *e, const char *sid, const char *op, const char *digest,
                              ah_error *error) {
    if (!digest[0])
        return AH_OK;
    sqlite3_int64 item = sqlite3_last_insert_rowid(e->db);
    sqlite3_stmt *s = NULL;
    ah_code code =
        core_prepare(e,
                     "INSERT OR REPLACE INTO observations(session_id,operation_id,digest,item_seq) "
                     "VALUES(?,?,?,?)",
                     &s, error);
    if (code == AH_OK) {
        sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, op, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, digest, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 4, item);
        code = core_done(e, s, error);
    }
    return code;
}
