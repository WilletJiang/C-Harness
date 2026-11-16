#include "tools/local_internal.h"
#include "core/content_internal.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>

static const char png[] =
    "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/"
    "x8AAwMCAO+aXioAAAAASUVORK5CYII=";

static void put(const char *path, const void *data, size_t size) {
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    assert(fd >= 0 && write(fd, data, size) == (ssize_t)size);
    close(fd);
}
static bool cancelled(void *context) {
    (void)context;
    return true;
}
typedef struct {
    const char *path;
    unsigned calls;
} mutation;
static bool change_file(void *context) {
    mutation *change = context;
    if (++change->calls == 2)
        put(change->path, "x", 1);
    return false;
}
static void check(ah_local_tools *tools, const char *args, size_t limit, ah_code expected,
                  ah_cancel_check cancel, void *context) {
    ah_tool_call call = {.arguments_json = args,
                         .output_limit = limit,
                         .deadline_ms = ah_now_ms() + 1000,
                         .cancelled = cancel,
                         .cancel_context = context};
    ah_tool_result result = {0};
    ah_error error = {0};
    assert(ah_local_view_image(tools, &call, &result, &error) == AH_OK);
    assert(result.json && strlen(result.json) <= limit && !result.outcome_unknown);
    yyjson_doc *doc = yyjson_read(result.json, strlen(result.json), 0);
    assert(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (expected == AH_OK) {
        assert(!result.is_error && result.format == AH_TOOL_RESULT_CONTENT);
        yyjson_val *picture = yyjson_arr_get(root, 1);
        assert(yyjson_equals_str(yyjson_obj_get(picture, "type"), "input_image"));
        const char *url = yyjson_get_str(yyjson_obj_get(picture, "image_url"));
        assert(url && !strcmp(url, png));
        assert(yyjson_equals_str(yyjson_obj_get(picture, "detail"), "high"));
    } else {
        assert(result.is_error && result.format == AH_TOOL_RESULT_JSON);
        if (limit >= 128)
            assert(yyjson_equals_str(yyjson_obj_get(root, "error"), ah_code_name(expected)));
    }
    yyjson_doc_free(doc);
    free(result.json);
}

int main(void) {
    char root[] = "/tmp/ah-view-image-XXXXXX";
    assert(mkdtemp(root));
    char path[256], link[256], pipe[256], directory[256];
    snprintf(path, sizeof(path), "%s/image.bin", root);
    snprintf(link, sizeof(link), "%s/link.png", root);
    snprintf(pipe, sizeof(pipe), "%s/pipe.png", root);
    snprintf(directory, sizeof(directory), "%s/directory", root);
    content_blob *blob = NULL;
    ah_error error = {0};
    assert(content_decode(png, NULL, &blob, &error) == AH_OK);
    put(path, blob->data, blob->size);
    assert(symlink(path, link) == 0 && mkfifo(pipe, 0600) == 0 && mkdir(directory, 0700) == 0);
    ah_local_tools_config config = {.workspace = root};
    ah_local_tools *tools = NULL;
    assert(ah_local_tools_open(&config, &tools, &error) == AH_OK);
    const char args[] = "{\"path\":\"image.bin\",\"detail\":\"high\"}";
    check(tools, args, 4096, AH_OK, NULL, NULL);
    check(tools, args, 64, AH_LIMIT, NULL, NULL);
    check(tools, args, 4096, AH_CANCELLED, cancelled, NULL);
    check(tools, "{\"path\":\"image.bin\",\"detail\":\"bad\"}", 4096, AH_INVALID, NULL, NULL);
    check(tools, "{\"path\":\"link.png\"}", 4096, AH_IO, NULL, NULL);
    check(tools, "{\"path\":\"../image.bin\"}", 4096, AH_IO, NULL, NULL);
    check(tools, "{\"path\":\"directory\"}", 4096, AH_INVALID, NULL, NULL);
    check(tools, "{\"path\":\"pipe.png\"}", 4096, AH_INVALID, NULL, NULL);
    mutation change = {.path = path};
    check(tools, args, 4096, AH_INVALID, change_file, &change);
    put(path, "not an image", 12);
    check(tools, args, 4096, AH_INVALID, NULL, NULL);
    int fd = open(path, O_WRONLY);
    assert(fd >= 0 && ftruncate(fd, AH_ARTIFACT_MAX_BYTES + 1) == 0);
    close(fd);
    check(tools, args, 16u * 1024u * 1024u, AH_LIMIT, NULL, NULL);
    ah_local_tools_close(tools);
    core_content content = {.blobs = blob};
    core_content_free(&content);
    assert(unlink(path) == 0 && unlink(link) == 0 && unlink(pipe) == 0);
    assert(rmdir(directory) == 0 && rmdir(root) == 0);
    return 0;
}
