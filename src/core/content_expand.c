#include "content_internal.h"

static ah_code expand_part(ah_engine *e, const char *sid, yyjson_mut_doc *d, yyjson_mut_val *part,
                           size_t *bytes, ah_error *err) {
    const char *id = yyjson_mut_get_str(yyjson_mut_obj_get(part, "artifact_id"));
    if (!id)
        return AH_OK;
    const char *type = yyjson_mut_get_str(yyjson_mut_obj_get(part, "type"));
    bool image = type && !strcmp(type, "input_image");
    if (!content_id_valid(id) || (!image && (!type || strcmp(type, "input_file"))))
        return ah_fail(err, AH_PROTOCOL, "corrupt stored artifact reference");
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(e,
                              "SELECT a.mime,length(CAST(a.data AS BLOB)),a.data FROM artifacts a "
                              "JOIN artifact_refs r ON a.id=r.artifact_id "
                              "WHERE r.session_id=? AND a.id=?",
                              &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, id, -1, SQLITE_TRANSIENT);
        int step = sqlite3_step(s);
        if (step != SQLITE_ROW)
            rc = ah_fail(err, step == SQLITE_DONE ? AH_NOT_FOUND : AH_IO,
                         "stored artifact is unavailable to this session");
        else {
            const char *mime = (const char *)sqlite3_column_text(s, 0);
            size_t size = (size_t)sqlite3_column_int64(s, 1);
            size_t expanded = (size + 2) / 3 * 4 + strlen(mime) + 13;
            if (expanded > e->config.max_context_bytes ||
                *bytes > e->config.max_context_bytes - expanded) {
                sqlite3_finalize(s);
                return ah_fail(err, AH_LIMIT,
                               "selected artifacts exceed model request byte budget");
            }
            const unsigned char *data = sqlite3_column_blob(s, 2);
            if (!content_mime_valid(mime, data, size) || (image && !content_image_mime(mime)))
                rc = ah_fail(err, AH_PROTOCOL, "corrupt stored artifact MIME");
            else {
                content_blob check = {
                    .mime = (char *)mime, .data = (unsigned char *)data, .size = size};
                content_blob_id(&check);
                if (strcmp(check.id, id))
                    rc = ah_fail(err, AH_PROTOCOL, "stored artifact digest mismatch");
                char *url = rc == AH_OK ? content_data_url(mime, data, size) : NULL;
                if (rc == AH_OK && !url)
                    rc = ah_fail(err, AH_IO, "out of memory");
                if (rc == AH_OK) {
                    yyjson_mut_obj_remove_key(part, "artifact_id");
                    yyjson_mut_obj_add_strcpy(d, part, image ? "image_url" : "file_data", url);
                    *bytes += expanded;
                }
                free(url);
            }
        }
    }
    sqlite3_finalize(s);
    return rc;
}
ah_code core_content_expand(ah_engine *e, const char *sid, yyjson_mut_doc *d, yyjson_mut_val *item,
                            size_t *bytes, ah_error *err) {
    const char *role = yyjson_mut_get_str(yyjson_mut_obj_get(item, "role"));
    yyjson_mut_val *parts = yyjson_mut_obj_get(item, "content");
    if (!role || strcmp(role, "user") || !yyjson_mut_is_arr(parts))
        return AH_OK;
    size_t i, n;
    yyjson_mut_val *part;
    yyjson_mut_arr_foreach(parts, i, n, part) {
        ah_code rc = expand_part(e, sid, d, part, bytes, err);
        if (rc != AH_OK)
            return rc;
    }
    return AH_OK;
}
