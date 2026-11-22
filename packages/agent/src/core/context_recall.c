#include "core.h"

ah_code core_context_recall(void *context, const ah_tool_call *call, ah_tool_result *result,
                            ah_error *error) {
    ah_engine *e = context;
    yyjson_doc *args = yyjson_read(call->arguments_json, strlen(call->arguments_json), 0);
    yyjson_val *root = yyjson_doc_get_root(args);
    const char *id = yyjson_get_str(yyjson_obj_get(root, "checkpoint"));
    sqlite3_int64 after = (sqlite3_int64)yyjson_get_num(yyjson_obj_get(root, "after_seq"));
    size_t offset = (size_t)yyjson_get_num(yyjson_obj_get(root, "offset"));
    yyjson_val *limit_value = yyjson_obj_get(root, "limit");
    size_t limit = limit_value ? (size_t)yyjson_get_num(limit_value) : 4096;
    size_t budget = call->model_output_limit;
    if (!budget || budget > call->output_limit)
        budget = call->output_limit;
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(e,
                              id ? "SELECT i.seq,i.json FROM context_checkpoint_refs r JOIN "
                                   "context_archive_items a ON a.checkpoint_id=r.checkpoint_id "
                                   "JOIN items i ON i.seq=a.item_seq WHERE r.session_id=? AND "
                                   "r.checkpoint_id=? AND i.seq>? ORDER BY i.seq LIMIT 1"
                                 : "SELECT c.rowid,c.id,c.summary FROM context_checkpoint_refs r "
                                   "JOIN context_checkpoints c ON c.id=r.checkpoint_id "
                                   "WHERE r.session_id=? AND c.rowid>? ORDER BY c.rowid LIMIT 13",
                              &s, error);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, call->session_id, -1, SQLITE_TRANSIENT);
        if (id) {
            sqlite3_bind_text(s, 2, id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 3, after);
        } else
            sqlite3_bind_int64(s, 2, after);
    }
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *out = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, out);
    int step = SQLITE_DONE;
    if (rc == AH_OK && id) {
        step = sqlite3_step(s);
        if (step == SQLITE_ROW) {
            const char *text = (const char *)sqlite3_column_text(s, 1);
            size_t total = strlen(text);
            if (offset > total || (offset < total && ((unsigned char)text[offset] & 0xc0) == 0x80))
                rc = ah_fail(error, AH_INVALID, "recall offset must be a UTF-8 boundary");
            if (rc == AH_OK) {
                if (limit > total - offset)
                    limit = total - offset;
                if (budget < 512)
                    rc = ah_fail(error, AH_LIMIT, "recall page budget too small");
                else {
                    if (limit > (budget - 512) / 6)
                        limit = (budget - 512) / 6;
                    while (limit && ((unsigned char)text[offset + limit] & 0xc0) == 0x80)
                        --limit;
                    if (!limit && offset < total)
                        rc = ah_fail(error, AH_LIMIT, "recall page cannot fit next character");
                    else {
                        yyjson_mut_obj_add_strcpy(doc, out, "checkpoint", id);
                        yyjson_mut_obj_add_sint(doc, out, "next_seq", sqlite3_column_int64(s, 0));
                        yyjson_mut_obj_add_uint(doc, out, "offset", offset);
                        yyjson_mut_obj_add_uint(doc, out, "next_offset", offset + limit);
                        yyjson_mut_obj_add_bool(doc, out, "more", offset + limit < total);
                        yyjson_mut_obj_add_strncpy(doc, out, "content", text + offset, limit);
                    }
                }
            }
        } else if (step == SQLITE_DONE)
            rc = ah_fail(error, AH_NOT_FOUND, "checkpoint item unavailable to this session");
        else
            rc = ah_fail(error, AH_IO, "cannot read archived context");
    } else if (rc == AH_OK) {
        yyjson_mut_val *list = yyjson_mut_arr(doc);
        yyjson_mut_obj_add_val(doc, out, "checkpoints", list);
        sqlite3_int64 last = after;
        bool more = false;
        while ((step = sqlite3_step(s)) == SQLITE_ROW) {
            if (yyjson_mut_arr_size(list) == 12) {
                more = true;
                break;
            }
            if (budget < 512) {
                rc = ah_fail(error, AH_LIMIT, "recall listing budget too small");
                break;
            }
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_sint(doc, item, "seq", sqlite3_column_int64(s, 0));
            yyjson_mut_obj_add_strcpy(doc, item, "id", (const char *)sqlite3_column_text(s, 1));
            const char *summary = (const char *)sqlite3_column_text(s, 2);
            size_t length = strlen(summary), cap = (budget - 256) / 6;
            if (cap > 128)
                cap = 128;
            if (length > cap)
                length = cap;
            while (length && ((unsigned char)summary[length] & 0xc0) == 0x80)
                --length;
            yyjson_mut_obj_add_strncpy(doc, item, "summary", summary, length);
            yyjson_mut_arr_append(list, item);
            char *encoded = ah_mut_write(doc);
            if (!encoded) {
                rc = ah_fail(error, AH_IO, "cannot encode checkpoint listing");
                break;
            }
            bool fits = strlen(encoded) <= budget - 128;
            free(encoded);
            if (!fits) {
                yyjson_mut_arr_remove_last(list);
                more = true;
                if (!yyjson_mut_arr_size(list))
                    rc = ah_fail(error, AH_LIMIT, "checkpoint identity exceeds listing budget");
                break;
            }
            last = sqlite3_column_int64(s, 0);
        }
        yyjson_mut_obj_add_sint(doc, out, "next_seq", last);
        yyjson_mut_obj_add_bool(doc, out, "more", more);
        if (rc == AH_OK && step != SQLITE_DONE && !more)
            rc = ah_fail(error, AH_IO, "cannot list archived context");
    }
    sqlite3_finalize(s);
    if (rc == AH_OK) {
        result->json = ah_mut_write(doc);
        if (!result->json)
            rc = ah_fail(error, AH_IO, "out of memory");
    }
    ah_mut_doc_free(doc);
    yyjson_doc_free(args);
    return rc;
}
