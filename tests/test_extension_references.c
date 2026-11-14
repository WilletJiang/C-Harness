#include "extensions/extensions_internal.h"
#include <assert.h>
#include <stdio.h>
#include <unistd.h>

static void write_reference(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    assert(file && fwrite(text, 1, strlen(text), file) == strlen(text));
    assert(fclose(file) == 0);
}

static yyjson_doc *read_reference(ah_engine *engine, const char *session, const char *id,
                                  const char *path, size_t offset, size_t limit, size_t budget,
                                  ah_code expected) {
    yyjson_mut_doc *args = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(args);
    yyjson_mut_doc_set_root(args, root);
    yyjson_mut_obj_add_str(args, root, "id", id);
    yyjson_mut_obj_add_str(args, root, "action", "read");
    yyjson_mut_obj_add_str(args, root, "path", path);
    yyjson_mut_obj_add_uint(args, root, "offset", offset);
    yyjson_mut_obj_add_uint(args, root, "limit", limit);
    char *json = ah_mut_write(args);
    ah_mut_doc_free(args);
    assert(json);
    ah_tool_call call = {.session_id = session,
                         .arguments_json = json,
                         .output_limit = budget,
                         .model_output_limit = budget};
    ah_tool_result result = {0};
    ah_error error = {0};
    ah_code code = ext_skill(engine->extensions, &call, &result, &error);
    if (code != expected)
        fprintf(stderr, "reference %s: expected %s, got %s: %s\n", path, ah_code_name(expected),
                ah_code_name(code), error.message);
    assert(code == expected);
    yyjson_doc *response = result.json ? yyjson_read(result.json, strlen(result.json), 0) : NULL;
    assert((code == AH_OK) == (response != NULL));
    free(result.json);
    free(json);
    return response;
}

void extension_reference_cases(ah_engine *engine, const char *session, const char *id,
                               const char *skill_path) {
    char reference[512], link[512];
    assert(strlen(skill_path) < sizeof(reference));
    strcpy(reference, skill_path);
    char *name = strrchr(reference, '/');
    assert(name);
    strcpy(name + 1, "reference.txt");
    strcpy(link, reference);
    strcpy(strrchr(link, '/') + 1, "escape");
    write_reference(reference, "A地球_REFERENCE_UNPINNED_185");
    yyjson_doc *page = read_reference(engine, session, id, "reference.txt", 1, 4, 4096, AH_OK);
    yyjson_val *root = yyjson_doc_get_root(page);
    assert(yyjson_equals_str(yyjson_obj_get(root, "content"), "地"));
    assert(yyjson_get_uint(yyjson_obj_get(root, "next_offset")) == 4);
    assert(yyjson_is_true(yyjson_obj_get(root, "more")));
    assert(yyjson_get_len(yyjson_obj_get(root, "revision")) == 64);
    yyjson_doc_free(page);
    page = read_reference(engine, session, id, "reference.txt", 4, 3, 4096, AH_OK);
    assert(yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(page), "content"), "球"));
    yyjson_doc_free(page);
    read_reference(engine, session, id, "reference.txt", 2, 4, 4096, AH_INVALID);
    read_reference(engine, session, id, "reference.txt", 1, 2, 4096, AH_LIMIT);
    read_reference(engine, session, id, "reference.txt", SIZE_MAX, 4, 4096, AH_INVALID);
    read_reference(engine, session, id, "reference.txt", 0, 100, 32, AH_LIMIT);
    read_reference(engine, session, id, "../secondary/SKILL.md", 0, 100, 4096, AH_INVALID);
    read_reference(engine, session, id, reference, 0, 100, 4096, AH_INVALID);
    assert(symlink("../secondary/SKILL.md", link) == 0);
    read_reference(engine, session, id, "escape", 0, 100, 4096, AH_INVALID);
    assert(unlink(link) == 0);
    write_reference(reference, "");
    page = read_reference(engine, session, id, "reference.txt", 0, 100, 4096, AH_OK);
    root = yyjson_doc_get_root(page);
    assert(yyjson_equals_str(yyjson_obj_get(root, "content"), ""));
    assert(yyjson_get_uint(yyjson_obj_get(root, "total_bytes")) == 0);
    assert(yyjson_is_false(yyjson_obj_get(root, "more")));
    yyjson_doc_free(page);
    assert(unlink(reference) == 0);
    core_session state;
    ah_error error = {0};
    char *request = NULL;
    assert(core_load(engine, session, &state, &error) == AH_OK);
    assert(core_request(engine, &state, &request, &error) == AH_OK);
    assert(!strstr(request, "REFERENCE_UNPINNED_185"));
    free(request);
    core_session_free(&state);
}
