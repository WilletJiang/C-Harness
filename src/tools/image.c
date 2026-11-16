#include "local_internal.h"
#include "../core/content_internal.h"

const char ah_image_schema[] =
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"minLength\":1,"
    "\"maxLength\":4096},\"detail\":{\"type\":\"string\","
    "\"enum\":[\"auto\",\"low\",\"high\",\"original\"]}},\"required\":[\"path\"],"
    "\"additionalProperties\":false}";

static const char *image_mime(const char *data, size_t size) {
    const char *types[] = {"image/png", "image/jpeg", "image/gif", "image/webp"};
    for (size_t i = 0; i < sizeof(types) / sizeof(*types); i++)
        if (content_mime_valid(types[i], (const unsigned char *)data, size))
            return types[i];
    return NULL;
}

ah_code ah_local_view_image(void *context, const ah_tool_call *call, ah_tool_result *result,
                            ah_error *error) {
    yyjson_doc *args = ah_local_arguments(call, ah_image_schema, result, error);
    if (!args)
        return result->json ? AH_OK : AH_IO;
    yyjson_val *root = yyjson_doc_get_root(args), *path = yyjson_obj_get(root, "path");
    const char *detail = yyjson_get_str(yyjson_obj_get(root, "detail"));
    char leaf[4097], *data = NULL;
    int parent = ah_local_parent_fd(context, path, leaf);
    size_t size = 0, budget = call->output_limit ? call->output_limit : AH_LOCAL_MAX_BYTES;
    size_t limit = budget / 4 * 3;
    if (limit > AH_ARTIFACT_MAX_BYTES)
        limit = AH_ARTIFACT_MAX_BYTES;
    struct stat original;
    ah_code code = parent < 0
                       ? AH_IO
                       : ah_local_bytes(parent, leaf, call, &original, &data, &size, limit, NULL);
    if (parent >= 0)
        close(parent);
    const char *mime = code == AH_OK ? image_mime(data, size) : NULL;
    if (code == AH_OK && !mime)
        code = AH_INVALID;
    yyjson_mut_doc *doc = NULL;
    if (code == AH_OK) {
        doc = ah_mut_doc_new();
        yyjson_mut_val *parts = yyjson_mut_arr(doc), *text = yyjson_mut_obj(doc),
                       *picture = yyjson_mut_obj(doc), *metadata = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, parts);
        char revision[65];
        ah_hash_bytes(data, size, revision);
        yyjson_mut_obj_add_strcpy(doc, metadata, "path", yyjson_get_str(path));
        yyjson_mut_obj_add_str(doc, metadata, "mime_type", mime);
        yyjson_mut_obj_add_uint(doc, metadata, "bytes", size);
        yyjson_mut_obj_add_strcpy(doc, metadata, "revision", revision);
        char *description = yyjson_mut_val_write(metadata, 0, NULL);
        char *url = content_data_url(mime, (const unsigned char *)data, size);
        if (!description || !url)
            code = AH_IO;
        else {
            yyjson_mut_obj_add_str(doc, text, "type", "input_text");
            yyjson_mut_obj_add_strcpy(doc, text, "text", description);
            yyjson_mut_obj_add_str(doc, picture, "type", "input_image");
            yyjson_mut_obj_add_strcpy(doc, picture, "image_url", url);
            yyjson_mut_obj_add_strcpy(doc, picture, "detail", detail ? detail : "auto");
            yyjson_mut_arr_append(parts, text);
            yyjson_mut_arr_append(parts, picture);
            result->json = ah_mut_write(doc);
            code = result->json ? AH_OK : AH_IO;
        }
        free(description);
        free(url);
        if (code == AH_OK && strlen(result->json) > budget)
            code = AH_LIMIT;
        if (code == AH_OK)
            code = ah_local_stopped(call);
    }
    free(data);
    ah_mut_doc_free(doc);
    yyjson_doc_free(args);
    if (code != AH_OK) {
        free(result->json);
        result->json = NULL;
        const char *message =
            code == AH_LIMIT
                ? "Image exceeds the output budget; use a smaller image or raise max_output_bytes"
                : "Cannot read a stable workspace PNG, JPEG, GIF or WebP image; symlinks are "
                  "refused";
        return ah_local_error(call, result, error, code, message);
    }
    result->format = AH_TOOL_RESULT_CONTENT;
    ah_error_clear(error);
    return AH_OK;
}
