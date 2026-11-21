#include "extensions_internal.h"
#include "../core/content_internal.h"
#include <stdio.h>

static bool media(yyjson_val *part) {
    const char *type = ext_string(part, "type");
    if (!type)
        return false;
    return !strcmp(type, "image") || !strcmp(type, "resource_link") ||
           (!strcmp(type, "resource") && yyjson_obj_get(yyjson_obj_get(part, "resource"), "blob"));
}

static ah_code binary_part(yyjson_mut_doc *doc, yyjson_mut_val *out, const char *mime,
                           const char *data, bool image, ah_error *error) {
    const char *suffix = !mime                               ? NULL
                         : !strcmp(mime, "application/pdf")  ? "pdf"
                         : !strcmp(mime, "text/plain")       ? "txt"
                         : !strcmp(mime, "application/json") ? "json"
                         : !strcmp(mime, "image/png")        ? "png"
                         : !strcmp(mime, "image/jpeg")       ? "jpg"
                         : !strcmp(mime, "image/gif")        ? "gif"
                         : !strcmp(mime, "image/webp")       ? "webp"
                                                             : NULL;
    if (!suffix || !data || !*data || (image && !content_image_mime(mime)))
        return ah_fail(error, AH_PROTOCOL, "unsupported MCP media MIME or missing data");
    size_t size = strlen(data) + strlen(mime) + 14;
    char *url = malloc(size);
    if (!url)
        return ah_fail(error, AH_IO, "cannot allocate MCP media");
    snprintf(url, size, "data:%s;base64,%s", mime, data);
    yyjson_mut_obj_add_str(doc, out, "type", image ? "input_image" : "input_file");
    yyjson_mut_obj_add_strcpy(doc, out, image ? "image_url" : "file_data", url);
    free(url);
    if (!image) {
        char filename[32];
        snprintf(filename, sizeof(filename), "mcp-resource.%s", suffix);
        yyjson_mut_obj_add_strcpy(doc, out, "filename", filename);
    }
    return AH_OK;
}

static ah_code convert_part(yyjson_val *part, yyjson_mut_doc *doc, yyjson_mut_val *parts,
                            yyjson_mut_val *metadata, ah_error *error) {
    const char *type = ext_string(part, "type");
    if (!type)
        return ah_fail(error, AH_PROTOCOL, "MCP content requires a type");
    yyjson_mut_val *out = yyjson_mut_obj(doc);
    ah_code rc = AH_OK;
    if (!strcmp(type, "image")) {
        rc = binary_part(doc, out, ext_string(part, "mimeType"), ext_string(part, "data"), true,
                         error);
        yyjson_mut_obj_remove_key(metadata, "data");
    } else if (!strcmp(type, "resource") &&
               yyjson_obj_get(yyjson_obj_get(part, "resource"), "blob")) {
        yyjson_val *resource = yyjson_obj_get(part, "resource");
        const char *mime = ext_string(resource, "mimeType");
        rc = binary_part(doc, out, mime, ext_string(resource, "blob"), content_image_mime(mime),
                         error);
        yyjson_mut_obj_remove_key(yyjson_mut_obj_get(metadata, "resource"), "blob");
    } else if (!strcmp(type, "resource_link") && ext_string(part, "uri") &&
               (!strncmp(ext_string(part, "uri"), "https://", 8) ||
                !strncmp(ext_string(part, "uri"), "http://", 7))) {
        bool image = content_image_mime(ext_string(part, "mimeType"));
        yyjson_mut_obj_add_str(doc, out, "type", image ? "input_image" : "input_file");
        yyjson_mut_obj_add_strcpy(doc, out, image ? "image_url" : "file_url",
                                  ext_string(part, "uri"));
    } else {
        const char *text = !strcmp(type, "text") ? ext_string(part, "text") : NULL;
        if (!strcmp(type, "resource"))
            text = ext_string(yyjson_obj_get(part, "resource"), "text");
        if (text) {
            if (!*text)
                return AH_OK;
            yyjson_mut_obj_remove_key(metadata, "text");
            yyjson_mut_obj_remove_key(yyjson_mut_obj_get(metadata, "resource"), "text");
        }
        char *encoded = text ? NULL : ah_json_write(part);
        if (!text && !encoded)
            return ah_fail(error, AH_IO, "cannot encode MCP content");
        yyjson_mut_obj_add_str(doc, out, "type", "input_text");
        yyjson_mut_obj_add_strcpy(doc, out, "text", text ? text : encoded);
        free(encoded);
    }
    if (rc == AH_OK)
        yyjson_mut_arr_append(parts, out);
    return rc;
}

/* This adapter is called only for an actual MCP tools/call response. Plain JSON
 * from arbitrary callbacks, and text-only MCP results, retain their old shape. */
ah_code ext_mcp_result(ah_tool_result *result, size_t limit, ah_error *error) {
    yyjson_doc *parsed = result->json ? yyjson_read(result->json, strlen(result->json), 0) : NULL;
    yyjson_val *root = yyjson_doc_get_root(parsed), *content = yyjson_obj_get(root, "content");
    if (!parsed || ah_json_check(root, error) != AH_OK) {
        yyjson_doc_free(parsed);
        return ah_fail(error, AH_PROTOCOL, "invalid MCP tool result");
    }
    result->is_error = yyjson_is_true(yyjson_obj_get(root, "isError"));
    bool native = false;
    size_t i, n;
    yyjson_val *part;
    yyjson_arr_foreach(content, i, n, part) native |= media(part);
    if (!native) {
        yyjson_doc_free(parsed);
        return AH_OK;
    }
    if (yyjson_arr_size(content) >= AH_CONTENT_MAX_PARTS) {
        yyjson_doc_free(parsed);
        return ah_fail(error, AH_LIMIT, "MCP content part limit exceeded");
    }
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *parts = yyjson_mut_arr(doc), *metadata = yyjson_val_mut_copy(doc, root),
                   *summaries = yyjson_mut_obj_get(metadata, "content");
    yyjson_mut_doc_set_root(doc, parts);
    ah_code rc = AH_OK;
    yyjson_arr_foreach(content, i, n, part) {
        rc = convert_part(part, doc, parts, yyjson_mut_arr_get(summaries, i), error);
        if (rc != AH_OK)
            break;
    }
    char *meta = rc == AH_OK ? yyjson_mut_val_write(metadata, 0, NULL) : NULL;
    if (rc == AH_OK && !meta)
        rc = ah_fail(error, AH_IO, "cannot encode MCP result metadata");
    if (rc == AH_OK) {
        yyjson_mut_val *out = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, out, "type", "input_text");
        yyjson_mut_obj_add_strcpy(doc, out, "text", meta);
        yyjson_mut_arr_prepend(parts, out);
    }
    free(meta);
    char *json = rc == AH_OK ? ah_mut_write(doc) : NULL;
    if (rc == AH_OK && (!json || strlen(json) > limit))
        rc = ah_fail(error, json ? AH_LIMIT : AH_IO, "MCP native result exceeds output budget");
    if (rc == AH_OK) {
        free(result->json);
        result->json = json;
        json = NULL;
        result->format = AH_TOOL_RESULT_CONTENT;
    }
    free(json);
    ah_mut_doc_free(doc);
    yyjson_doc_free(parsed);
    return rc;
}
