#include "content_internal.h"
#include <ctype.h>
#include <curl/curl.h>
#include <stdio.h>

void core_content_free(core_content *content) {
    free(content->json);
    content_blob *blob = content->blobs;
    while (blob) {
        content_blob *next = blob->next;
        free(blob->data);
        free(blob->mime);
        free(blob);
        blob = next;
    }
    memset(content, 0, sizeof(*content));
}
static bool file_id_valid(const char *id) {
    if (!id || strncmp(id, "file-", 5) || !id[5] || strlen(id) > 256)
        return false;
    for (const unsigned char *p = (const unsigned char *)id + 5; *p; p++)
        if (!isalnum(*p) && *p != '-' && *p != '_')
            return false;
    return true;
}
static bool filename_valid(const char *name) {
    if (!name || !*name || strlen(name) > 255 || !strcmp(name, ".") || !strcmp(name, ".."))
        return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (*p < 32 || *p == 127 || *p == '/' || *p == '\\')
            return false;
    return true;
}
static bool url_valid(const char *url) {
    if (!url || !*url || strlen(url) > 8192)
        return false;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
        if (*p <= 32 || *p == 127 || *p == '\\')
            return false;
        if (*p == '%' && (!p[1] || !p[2] || !isxdigit(p[1]) || !isxdigit(p[2])))
            return false;
    }
    CURLU *u = curl_url();
    char *scheme = NULL, *host = NULL, *user = NULL, *password = NULL;
    bool valid = u && curl_url_set(u, CURLUPART_URL, url, 0) == CURLUE_OK &&
                 curl_url_get(u, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
                 (!strcmp(scheme, "https") || !strcmp(scheme, "http")) &&
                 curl_url_get(u, CURLUPART_HOST, &host, 0) == CURLUE_OK && *host &&
                 curl_url_get(u, CURLUPART_USER, &user, 0) == CURLUE_NO_USER &&
                 curl_url_get(u, CURLUPART_PASSWORD, &password, 0) == CURLUE_NO_PASSWORD;
    curl_free(scheme);
    curl_free(host);
    curl_free(user);
    curl_free(password);
    if (u)
        curl_url_cleanup(u);
    return valid;
}
static ah_code fields_valid(yyjson_val *part, const char *const *allowed, size_t count,
                            ah_error *err) {
    size_t i, n;
    yyjson_val *key, *value;
    yyjson_obj_foreach(part, i, n, key, value) {
        const char *name = yyjson_get_str(key);
        bool found = false;
        for (size_t j = 0; j < count; j++)
            found |= !strcmp(name, allowed[j]);
        if (!found || !yyjson_is_str(value))
            return ah_fail(err, AH_INVALID, "unknown content field or nonstring value");
    }
    return AH_OK;
}
static ah_code media_part(ah_engine *e, const char *sid, yyjson_val *part, yyjson_mut_doc *d,
                          yyjson_mut_val *out, core_content *content, bool image, ah_error *err) {
    static const char *const image_fields[] = {"type", "image_url", "file_id", "artifact_id",
                                               "detail"};
    static const char *const file_fields[] = {"type",        "file_url", "file_id", "file_data",
                                              "artifact_id", "filename", "detail"};
    ah_code rc = fields_valid(part, image ? image_fields : file_fields,
                              image ? sizeof(image_fields) / sizeof(*image_fields)
                                    : sizeof(file_fields) / sizeof(*file_fields),
                              err);
    if (rc != AH_OK)
        return rc;
    const char *url = yyjson_get_str(yyjson_obj_get(part, image ? "image_url" : "file_url"));
    const char *id = yyjson_get_str(yyjson_obj_get(part, "file_id"));
    const char *artifact = yyjson_get_str(yyjson_obj_get(part, "artifact_id"));
    const char *data = yyjson_get_str(yyjson_obj_get(part, "file_data"));
    const char *filename = yyjson_get_str(yyjson_obj_get(part, "filename"));
    const char *detail = yyjson_get_str(yyjson_obj_get(part, "detail"));
    if ((!!url + !!id + !!artifact + !!data) != 1)
        return ah_fail(err, AH_INVALID, "content requires exactly one media source");
    if (detail && strcmp(detail, "auto") && strcmp(detail, "low") && strcmp(detail, "high") &&
        (!image || strcmp(detail, "original")))
        return ah_fail(err, AH_INVALID, "invalid content detail");
    if (!image &&
        ((data || artifact) ? !filename_valid(filename) : filename && !filename_valid(filename)))
        return ah_fail(err, AH_INVALID, "local file content requires a bounded filename");
    yyjson_mut_obj_add_str(d, out, "type", image ? "input_image" : "input_file");
    yyjson_mut_obj_add_strcpy(d, out, "detail", detail ? detail : "auto");
    if (filename)
        yyjson_mut_obj_add_strcpy(d, out, "filename", filename);
    if (id) {
        if (!file_id_valid(id))
            return ah_fail(err, AH_INVALID, "invalid provider file_id");
        yyjson_mut_obj_add_strcpy(d, out, "file_id", id);
    } else if (artifact) {
        if (!content_id_valid(artifact))
            return ah_fail(err, AH_INVALID, "invalid artifact_id");
        rc = content_reference(e, sid, artifact, image, err);
        if (rc == AH_OK)
            yyjson_mut_obj_add_strcpy(d, out, "artifact_id", artifact);
    } else if (data || (image && !strncmp(url, "data:", 5))) {
        content_blob *blob = NULL;
        rc = content_decode(data ? data : url, content_filename_mime(filename), &blob, err);
        if (rc == AH_OK) {
            blob->next = content->blobs;
            content->blobs = blob;
            if (image && !content_image_mime(blob->mime))
                rc = ah_fail(err, AH_INVALID, "input_image requires image MIME");
            else
                yyjson_mut_obj_add_strcpy(d, out, "artifact_id", blob->id);
        }
    } else {
        if (!url_valid(url))
            return ah_fail(err, AH_INVALID,
                           "media URL must be a bounded HTTP(S) URL without credentials");
        yyjson_mut_obj_add_strcpy(d, out, image ? "image_url" : "file_url", url);
    }
    return rc;
}
ah_code core_content_parts_prepare(ah_engine *e, const char *sid, const char *json, size_t limit,
                                   core_content *content, ah_error *err) {
    memset(content, 0, sizeof(*content));
    if (!json || !*json || strlen(json) > limit)
        return ah_fail(err, AH_INVALID, "bounded nonempty content array required");
    yyjson_doc *parsed = yyjson_read(json, strlen(json), 0);
    yyjson_val *array = yyjson_doc_get_root(parsed);
    ah_code rc = parsed ? ah_json_check(array, err) : AH_INVALID;
    if (rc == AH_OK && (!yyjson_is_arr(array) || !yyjson_arr_size(array) ||
                        yyjson_arr_size(array) > AH_CONTENT_MAX_PARTS))
        rc = AH_INVALID;
    if (rc != AH_OK) {
        yyjson_doc_free(parsed);
        return ah_fail(err, AH_INVALID, "invalid bounded content array");
    }
    yyjson_mut_doc *d = ah_mut_doc_new();
    if (!d) {
        yyjson_doc_free(parsed);
        return ah_fail(err, AH_IO, "out of memory");
    }
    yyjson_mut_val *parts = yyjson_mut_arr(d);
    yyjson_mut_doc_set_root(d, parts);
    size_t i, n;
    yyjson_val *part;
    yyjson_arr_foreach(array, i, n, part) {
        if (rc != AH_OK)
            break;
        const char *type = yyjson_get_str(yyjson_obj_get(part, "type"));
        yyjson_mut_val *out = yyjson_mut_obj(d);
        if (!type || !yyjson_is_obj(part))
            rc = ah_fail(err, AH_INVALID, "content part requires a type");
        else if (!strcmp(type, "input_text")) {
            static const char *const fields[] = {"type", "text"};
            rc = fields_valid(part, fields, 2, err);
            const char *text = yyjson_get_str(yyjson_obj_get(part, "text"));
            if (rc == AH_OK && (!text || !*text))
                rc = ah_fail(err, AH_INVALID, "input_text requires nonempty text");
            if (rc == AH_OK) {
                yyjson_mut_obj_add_str(d, out, "type", "input_text");
                yyjson_mut_obj_add_strcpy(d, out, "text", text);
            }
        } else if (!strcmp(type, "input_image") || !strcmp(type, "input_file"))
            rc = media_part(e, sid, part, d, out, content, !strcmp(type, "input_image"), err);
        else
            rc = ah_fail(err, AH_INVALID, "unsupported input content type");
        if (rc == AH_OK)
            yyjson_mut_arr_append(parts, out);
    }
    if (rc == AH_OK && !(content->json = ah_mut_write(d)))
        rc = ah_fail(err, AH_IO, "out of memory");
    ah_mut_doc_free(d);
    yyjson_doc_free(parsed);
    if (rc != AH_OK)
        core_content_free(content);
    return rc;
}
ah_code core_content_prepare(ah_engine *e, const char *sid, const char *json, core_content *content,
                             ah_error *err) {
    ah_code rc =
        core_content_parts_prepare(e, sid, json, e->config.max_context_bytes, content, err);
    if (rc != AH_OK)
        return rc;
    size_t size = strlen(content->json) + sizeof("{\"role\":\"user\",\"content\":}");
    char *item = malloc(size);
    if (!item) {
        core_content_free(content);
        return ah_fail(err, AH_IO, "out of memory");
    }
    snprintf(item, size, "{\"role\":\"user\",\"content\":%s}", content->json);
    free(content->json);
    content->json = item;
    return AH_OK;
}
ah_code ah_submit_content(ah_engine *e, const char *sid, const char *command, const char *json,
                          ah_error *err) {
    ah_code rc = core_guard(e, err);
    if (rc != AH_OK)
        return rc;
    if (!sid || !command || !*command || strlen(command) > 256)
        return ah_fail(err, AH_INVALID, "session and bounded command id required");
    core_content content;
    rc = core_content_prepare(e, sid, json, &content, err);
    if (rc == AH_OK)
        rc = core_submit_item(e, sid, command, content.json, 1, &content, err);
    core_content_free(&content);
    return rc;
}
