#include "content_internal.h"

/* Called only by the owner inside the result-capture transaction. */
ah_code core_normalize_result(ah_engine *e, const char *sid, core_operation *op,
                              ah_tool_result *result, ah_error *err) {
    if (result->format != AH_TOOL_RESULT_JSON && result->format != AH_TOOL_RESULT_CONTENT)
        return ah_fail(err, AH_INVALID, "unknown tool result format");
    if (!result->json || strlen(result->json) > e->config.max_output_bytes ||
        !ah_json_valid(result->json))
        return ah_fail(err, AH_INVALID, "bounded valid tool result required");
    if (result->format == AH_TOOL_RESULT_JSON) {
        op->result_format = AH_TOOL_RESULT_JSON;
        return AH_OK;
    }
    if (sqlite3_get_autocommit(e->db))
        return ah_fail(err, AH_INVALID, "content result capture requires owner transaction");
    core_content content;
    ah_code rc =
        core_content_parts_prepare(e, sid, result->json, e->config.max_output_bytes, &content, err);
    if (rc == AH_OK && strlen(content.json) > e->config.max_output_bytes)
        rc = ah_fail(err, AH_LIMIT, "normalized content exceeds tool output budget");
    if (rc == AH_OK)
        rc = core_content_save(e, sid, &content, err);
    if (rc == AH_OK) {
        free(result->json);
        result->json = content.json;
        content.json = NULL;
        op->result_format = AH_TOOL_RESULT_CONTENT;
    }
    core_content_free(&content);
    return rc;
}

static size_t text_prefix(const char *text, size_t length) {
    while (length && ((unsigned char)text[length] & 0xc0) == 0x80)
        length--;
    return length;
}

static bool add_text(yyjson_mut_doc *doc, yyjson_mut_val *parts, const char *text, size_t length) {
    yyjson_mut_val *part = yyjson_mut_obj(doc);
    return part && yyjson_mut_obj_add_str(doc, part, "type", "input_text") &&
           yyjson_mut_obj_add_strncpy(doc, part, "text", text, length) &&
           yyjson_mut_arr_append(parts, part);
}

/* The model-view bound covers text and immutable references. Binary expansion
 * has its own whole-request context bound. Keep media when shortening text. */
char *core_content_result_view(ah_engine *e, const core_operation *op, const char *json,
                               bool is_error, ah_error *err) {
    yyjson_doc *parsed = yyjson_read(json, strlen(json), 0);
    yyjson_val *source = yyjson_doc_get_root(parsed);
    if (!yyjson_is_arr(source)) {
        yyjson_doc_free(parsed);
        ah_fail(err, AH_PROTOCOL, "corrupt native tool result");
        return NULL;
    }
    size_t budget = e->config.max_model_tool_bytes, text_limit = strlen(json);
    bool truncated = false, deferred = false;
    char *out = NULL;
    for (;;) {
        yyjson_mut_doc *doc = ah_mut_doc_new();
        if (!doc)
            break;
        yyjson_mut_val *parts = yyjson_mut_arr(doc), *meta = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, parts);
        yyjson_mut_obj_add_bool(doc, meta, "is_error", is_error);
        if (truncated || deferred) {
            yyjson_mut_obj_add_bool(doc, meta, "truncated", true);
            yyjson_mut_obj_add_strcpy(doc, meta, "operation_id", op->id);
            yyjson_mut_obj_add_str(doc, meta, "retrieval_tool", "read_result");
            yyjson_mut_obj_add_str(doc, meta, "result_format", "content");
            if (deferred)
                yyjson_mut_obj_add_bool(doc, meta, "media_deferred", true);
        }
        char *metadata = yyjson_mut_val_write(meta, 0, NULL);
        bool valid = metadata && add_text(doc, parts, metadata, strlen(metadata));
        free(metadata);
        size_t i, n;
        yyjson_val *part;
        if (!deferred)
            yyjson_arr_foreach(source, i, n, part) {
                const char *type = yyjson_get_str(yyjson_obj_get(part, "type"));
                if (type && !strcmp(type, "input_text")) {
                    const char *text = yyjson_get_str(yyjson_obj_get(part, "text"));
                    size_t length = text ? strlen(text) : 0;
                    if (length > text_limit)
                        length = text_prefix(text, text_limit);
                    if (length)
                        valid &= add_text(doc, parts, text, length);
                } else
                    valid &= yyjson_mut_arr_append(parts, yyjson_val_mut_copy(doc, part));
            }
        out = valid ? ah_mut_write(doc) : NULL;
        ah_mut_doc_free(doc);
        if (!out || !budget || strlen(out) <= budget)
            break;
        free(out);
        out = NULL;
        if (deferred)
            break;
        if (!text_limit)
            deferred = true;
        text_limit /= 2;
        truncated = true;
    }
    yyjson_doc_free(parsed);
    if (!out)
        ah_fail(err, AH_IO, "cannot encode native tool receipt");
    return out;
}
