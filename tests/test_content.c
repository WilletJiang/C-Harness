#include "core/content_internal.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static const char done[] = "{\"status\":\"completed\",\"output\":[]}";
static const char image[] =
    "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/"
    "x8AAwMCAO+aXioAAAAASUVORK5CYII=";
typedef struct {
    char *request;
} fixture;
static void ok(ah_code rc, const ah_error *err) {
    if (rc != AH_OK) {
        fprintf(stderr, "%s: %s\n", ah_code_name(rc), err->message);
        abort();
    }
}
static ah_code complete(void *ctx, const char *request, ah_delta_sink delta, void *dc,
                        ah_cancel_check cancel, void *cc, char **response, ah_error *err) {
    (void)delta;
    (void)dc;
    (void)cancel;
    (void)cc;
    (void)err;
    fixture *f = ctx;
    free(f->request);
    f->request = ah_strdup(request);
    *response = ah_strdup(done);
    return AH_OK;
}
static ah_engine *open_engine(fixture *f, const char *path) {
    ah_config config;
    ah_config_init(&config);
    config.database_path = path;
    config.provider = (ah_provider){complete, f};
    ah_engine *e = NULL;
    ah_error err = {0};
    ok(ah_open(&config, &e, &err), &err);
    return e;
}
static int count(ah_engine *e, const char *sql) {
    sqlite3_stmt *s = NULL;
    assert(sqlite3_prepare_v2(e->db, sql, -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    int value = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return value;
}
static void invalid_inputs(void) {
    fixture f = {0};
    ah_engine *e = open_engine(&f, ":memory:");
    ah_error err = {0};
    char sid[AH_ID_CAP];
    ok(ah_session_create(e, "mock", "", sid, &err), &err);
    const char *bad[] = {
        "{}",
        "[]",
        "null",
        "[null]",
        "[{\"type\":\"unknown\"}]",
        "[{\"type\":\"input_text\",\"text\":\"\"}]",
        "[{\"type\":\"input_text\",\"text\":\"a\",\"text\":\"b\"}]",
        "[{\"type\":\"input_text\",\"text\":\"a\\u0000b\"}]",
        "[{\"type\":\"input_text\",\"text\":\"\xff\"}]",
        "[{\"type\":\"input_image\",\"image_url\":\"file:///secret\"}]",
        "[{\"type\":\"input_image\",\"image_url\":\"https://u:p@example.com/a\"}]",
        "[{\"type\":\"input_image\",\"image_url\":\"https://example.com/%\"}]",
        ("[{\"type\":\"input_image\",\"image_url\":\"https://example.com/"
         "a\",\"file_id\":\"file-x\"}]"),
        "[{\"type\":\"input_image\",\"file_id\":\"file-\"}]",
        "[{\"type\":\"input_file\",\"file_id\":\"file-x\",\"detail\":\"original\"}]",
        "[{\"type\":\"input_file\",\"file_data\":\"YQ==\",\"filename\":\"../a.txt\"}]",
        "[{\"type\":\"input_file\",\"file_data\":\"YQ==\"}]",
        "[{\"type\":\"input_file\",\"file_data\":\"YQ==\",\"filename\":\"a.zip\"}]",
        "[{\"type\":\"input_image\",\"image_url\":\"data:text/plain;base64,YQ==\"}]",
        "[{\"type\":\"input_image\",\"file_id\":null}]",
        "[{\"type\":\"input_text\",\"text\":\"x\",\"extra\":\"x\"}]"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        assert(ah_submit_content(e, sid, "bad", bad[i], &err) == AH_INVALID);
        assert(count(e, "SELECT count(*) FROM commands") == 0);
        assert(count(e, "SELECT count(*) FROM artifacts") == 0);
    }
    e->config.max_context_bytes = 4;
    assert(ah_submit_content(e, sid, "bad", "[{}] ", &err) == AH_INVALID);
    ah_close(e);
    free(f.request);
}
static void codecs(void) {
    ah_error err = {0};
    const char *bad[] = {
        "", "YQ", "YQ=", "YR==", "YWF=", "=Q==", "YQ==YQ==", "Y Q=", "YQ=_", "YQ==\n"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        content_blob *blob = NULL;
        assert(content_decode(bad[i], "text/plain", &blob, &err) != AH_OK);
        assert(!blob);
    }
    const char text[] = "hello 世界";
    for (size_t n = 1; n <= 6; n++) {
        char *url = content_data_url("text/plain", (const unsigned char *)text, n);
        content_blob *blob = NULL;
        ok(content_decode(url, NULL, &blob, &err), &err);
        assert(blob->size == n && !memcmp(blob->data, text, n));
        core_content content = {.blobs = blob};
        core_content_free(&content);
        free(url);
    }
    assert(!content_mime_valid("text/plain", (const unsigned char *)"\xc3", 1));
    assert(!content_mime_valid("image/png", (const unsigned char *)"not an image", 12));
    assert(!content_mime_valid("application/json", (const unsigned char *)"{", 1));
}
static void artifact_bound_and_integrity(void) {
    fixture f = {0};
    ah_engine *e = open_engine(&f, ":memory:");
    ah_error err = {0};
    char sid[AH_ID_CAP];
    ok(ah_session_create(e, "mock", "", sid, &err), &err);
    const char *input = "[{\"type\":\"input_file\",\"file_data\":\"YWJj\",\"filename\":\"a.txt\"}]";
    ok(core_sql(e,
                "CREATE TRIGGER reject_reference BEFORE INSERT ON artifact_refs BEGIN "
                "SELECT RAISE(ABORT,'injected failure'); END",
                &err),
       &err);
    int events = count(e, "SELECT count(*) FROM events");
    assert(ah_submit_content(e, sid, "binary", input, &err) == AH_INVALID);
    assert(count(e, "SELECT count(*) FROM artifacts") == 0);
    assert(count(e, "SELECT count(*) FROM commands") == 0);
    assert(count(e, "SELECT count(*) FROM events") == events);
    ok(core_sql(e, "DROP TRIGGER reject_reference", &err), &err);
    ok(ah_submit_content(e, sid, "binary", input, &err), &err);
    sqlite3_stmt *s = NULL;
    ok(core_prepare(e, "SELECT json FROM items", &s, &err), &err);
    assert(sqlite3_step(s) == SQLITE_ROW);
    const char *json = (const char *)sqlite3_column_text(s, 0);
    yyjson_doc *parsed = yyjson_read(json, strlen(json), 0);
    assert(parsed);
    sqlite3_finalize(s);
    yyjson_mut_doc *d = ah_mut_doc_new();
    yyjson_mut_val *item = yyjson_val_mut_copy(d, yyjson_doc_get_root(parsed));
    yyjson_mut_doc_set_root(d, item);
    size_t bytes = 0, limit = e->config.max_context_bytes;
    e->config.max_context_bytes = 1;
    assert(core_content_expand(e, sid, d, item, &bytes, &err) == AH_LIMIT && bytes == 0);
    e->config.max_context_bytes = limit;
    ok(core_content_expand(e, sid, d, item, &bytes, &err), &err);
    char *expanded = ah_mut_write(d);
    assert(expanded && strstr(expanded, "data:text/plain;base64,YWJj"));
    free(expanded);
    ah_mut_doc_free(d);
    ok(core_sql(e, "UPDATE artifacts SET data=CAST('xyz' AS BLOB)", &err), &err);
    d = ah_mut_doc_new();
    item = yyjson_val_mut_copy(d, yyjson_doc_get_root(parsed));
    bytes = 0;
    assert(core_content_expand(e, sid, d, item, &bytes, &err) == AH_PROTOCOL && bytes == 0);
    assert(strstr(err.message, "digest mismatch"));
    ah_mut_doc_free(d);
    yyjson_doc_free(parsed);
    ah_close(e);
    free(f.request);
}
static void typed_history(void) {
    fixture f = {0};
    ah_engine *e = open_engine(&f, ":memory:");
    ah_error err = {0};
    char sid[AH_ID_CAP], other[AH_ID_CAP], fork[AH_ID_CAP];
    ok(ah_session_create(e, "mock", "", sid, &err), &err);
    char input[1024];
    snprintf(input, sizeof(input),
             "[{\"type\":\"input_text\",\"text\":\"inspect\"},"
             "{\"type\":\"input_image\",\"image_url\":\"%s\"},"
             "{\"type\":\"input_image\",\"image_url\":\"%s\"}]",
             image, image);
    ok(ah_submit_content(e, sid, "typed", input, &err), &err);
    assert(count(e, "SELECT count(*) FROM artifacts") == 1);
    assert(count(e, "SELECT count(*) FROM artifact_refs") == 1);
    int events = count(e, "SELECT count(*) FROM events");
    ok(ah_submit_content(e, sid, "typed", input, &err), &err);
    assert(count(e, "SELECT count(*) FROM events") == events);
    assert(count(e, "SELECT count(*) FROM events WHERE instr(json,'base64')") == 0);
    assert(count(e, "SELECT count(*) FROM commands WHERE instr(text,'base64')") == 0);
    assert(count(e, "SELECT count(*) FROM items WHERE instr(json,'base64')") == 0);
    char *catalog = NULL;
    ok(ah_artifacts_json(e, sid, &catalog, &err), &err);
    yyjson_doc *doc = yyjson_read(catalog, strlen(catalog), 0);
    const char *artifact =
        yyjson_get_str(yyjson_obj_get(yyjson_arr_get(yyjson_doc_get_root(doc), 0), "artifact_id"));
    assert(artifact && strlen(artifact) == 64);
    char reference[256];
    snprintf(reference, sizeof(reference), "[{\"type\":\"input_image\",\"artifact_id\":\"%s\"}]",
             artifact);
    ok(ah_session_create(e, "mock", "", other, &err), &err);
    assert(ah_submit_content(e, other, "private", reference, &err) == AH_NOT_FOUND);
    assert(ah_submit_content(e, sid, "typed", reference, &err) == AH_INVALID);
    assert(ah_submit(e, sid, "typed", input, &err) == AH_INVALID);
    ok(ah_run(e, sid, &err), &err);
    assert(strstr(f.request, image) && !strstr(f.request, "artifact_id"));
    ok(ah_fork(e, sid, fork, &err), &err);
    ok(ah_submit_content(e, fork, "inherited", reference, &err), &err);
    ok(ah_run(e, fork, &err), &err);
    assert(strstr(f.request, image));
    ok(ah_compact(e, fork, "summary", 0, &err), &err);
    ok(ah_submit(e, fork, "text", "next", &err), &err);
    ok(ah_run(e, fork, &err), &err);
    assert(!strstr(f.request, "base64"));
    ok(ah_submit_content(e, fork, "again", reference, &err), &err);
    ok(ah_run(e, fork, &err), &err);
    assert(strstr(f.request, image));
    free(catalog);
    yyjson_doc_free(doc);
    ah_close(e);
    free(f.request);
}
static void native_sources_and_atomicity(void) {
    fixture f = {0};
    ah_engine *e = open_engine(&f, ":memory:");
    ah_error err = {0};
    char sid[AH_ID_CAP];
    ok(ah_session_create(e, "mock", "", sid, &err), &err);
    const char *content =
        "[{\"type\":\"input_image\",\"image_url\":\"https://example.com/"
        "image.png\",\"detail\":\"original\"},"
        "{\"type\":\"input_image\",\"file_id\":\"file-image\"},"
        "{\"type\":\"input_file\",\"file_id\":\"file-document\",\"detail\":\"low\"},"
        "{\"type\":\"input_file\",\"file_url\":\"https://example.com/document.pdf\"}]";
    ok(ah_submit_content(e, sid, "native", content, &err), &err);
    ok(ah_run(e, sid, &err), &err);
    assert(strstr(f.request, "https://example.com/image.png"));
    assert(strstr(f.request, "file-image") && strstr(f.request, "file-document"));
    assert(strstr(f.request, "\"file_url\":\"https://example.com/document.pdf\""));
    const char *raw = "[{\"type\":\"input_file\",\"file_data\":\"YQ==\",\"filename\":\"a.txt\"}]";
    const char *reordered =
        "[ { \"filename\": \"a.txt\", \"file_data\": \"YQ==\", \"type\": \"input_file\" } ]";
    ok(ah_submit_content(e, sid, "canonical", raw, &err), &err);
    int events = count(e, "SELECT count(*) FROM events");
    ok(ah_submit_content(e, sid, "canonical", reordered, &err), &err);
    assert(count(e, "SELECT count(*) FROM events") == events);
    const char *bad_after_blob =
        "[{\"type\":\"input_file\",\"file_data\":\"Yg==\",\"filename\":\"b.txt\"},{}]";
    assert(ah_submit_content(e, sid, "atomic", bad_after_blob, &err) == AH_INVALID);
    assert(count(e, "SELECT count(*) FROM artifacts") == 1);
    assert(count(e, "SELECT count(*) FROM events") == events);
    const char *canonical_text =
        "{\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"x\"}]}";
    ok(ah_submit(e, sid, "plain-kind", canonical_text, &err), &err);
    assert(ah_submit_content(e, sid, "plain-kind", "[{\"type\":\"input_text\",\"text\":\"x\"}]",
                             &err) == AH_INVALID);
    yyjson_mut_doc *d = ah_mut_doc_new();
    yyjson_mut_val *array = yyjson_mut_arr(d);
    yyjson_mut_doc_set_root(d, array);
    for (unsigned i = 0; i <= AH_CONTENT_MAX_PARTS; i++) {
        yyjson_mut_val *part = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, part, "type", "input_text");
        yyjson_mut_obj_add_str(d, part, "text", "x");
        yyjson_mut_arr_append(array, part);
    }
    char *many = ah_mut_write(d);
    assert(many && ah_submit_content(e, sid, "many", many, &err) == AH_INVALID);
    free(many);
    ah_mut_doc_free(d);
    e->config.max_context_bytes = 1;
    core_session session;
    ok(core_load(e, sid, &session, &err), &err);
    char *request = NULL;
    assert(core_request(e, &session, &request, &err) == AH_LIMIT && !request);
    core_session_free(&session);
    ah_close(e);
    free(f.request);
}
static void import_and_reopen(void) {
    char database[] = "/tmp/ah-content-db-XXXXXX", path[] = "/tmp/ah-content-file-XXXXXX";
    int dbfd = mkstemp(database), fd = mkstemp(path);
    assert(dbfd >= 0 && fd >= 0);
    close(dbfd);
    assert(write(fd, "hello world", 11) == 11);
    close(fd);
    fixture f = {0};
    ah_engine *e = open_engine(&f, database);
    ah_error err = {0};
    char sid[AH_ID_CAP], id[AH_ARTIFACT_ID_CAP], duplicate[AH_ARTIFACT_ID_CAP];
    ok(ah_session_create(e, "mock", "", sid, &err), &err);
    ok(ah_artifact_import(e, sid, path, "text/plain", id, &err), &err);
    ok(ah_artifact_import(e, sid, path, "text/plain", duplicate, &err), &err);
    assert(!strcmp(id, duplicate) && count(e, "SELECT count(*) FROM artifacts") == 1);
    ok(ah_submit(e, sid, "catalog", "do not select any artifact", &err), &err);
    ok(ah_run(e, sid, &err), &err);
    assert(!strstr(f.request, "base64"));
    assert(ah_artifact_import(e, sid, path, "image/png", duplicate, &err) == AH_INVALID);
    char *directory = realpath("/tmp", NULL);
    assert(directory);
    assert(ah_artifact_import(e, sid, directory, "text/plain", duplicate, &err) == AH_INVALID);
    free(directory);
    char link[sizeof(path) + 8];
    snprintf(link, sizeof(link), "%s.link", path);
    assert(symlink(path, link) == 0);
    assert(ah_artifact_import(e, sid, link, "text/plain", duplicate, &err) == AH_IO);
    unlink(link);
    snprintf(link, sizeof(link), "%s.fifo", path);
    assert(mkfifo(link, 0600) == 0);
    assert(ah_artifact_import(e, sid, link, "text/plain", duplicate, &err) == AH_INVALID);
    unlink(link);
    fd = open(path, O_WRONLY);
    assert(fd >= 0 && ftruncate(fd, 0) == 0);
    assert(ah_artifact_import(e, sid, path, "text/plain", duplicate, &err) == AH_INVALID);
    assert(ftruncate(fd, AH_ARTIFACT_MAX_BYTES + 1) == 0);
    close(fd);
    assert(ah_artifact_import(e, sid, path, "text/plain", duplicate, &err) == AH_LIMIT);
    ah_close(e);
    e = open_engine(&f, database);
    char input[256];
    snprintf(input, sizeof(input),
             "[{\"type\":\"input_file\",\"artifact_id\":\"%s\",\"filename\":\"hello.txt\"}]", id);
    ok(ah_submit_content(e, sid, "file", input, &err), &err);
    ok(ah_run(e, sid, &err), &err);
    assert(strstr(f.request, "data:text/plain;base64,aGVsbG8gd29ybGQ="));
    ok(ah_submit_content(e, sid, "raw",
                         "[{\"type\":\"input_file\",\"file_data\":\"aGVsbG8gd29ybGQ=\","
                         "\"filename\":\"hello.txt\"}]",
                         &err),
       &err);
    assert(count(e, "SELECT count(*) FROM artifacts") == 1);
    ah_close(e);
    e = open_engine(&f, database);
    int events = count(e, "SELECT count(*) FROM events");
    ok(ah_submit_content(e, sid, "file", input, &err), &err);
    assert(count(e, "SELECT count(*) FROM events") == events);
    ah_close(e);
    free(f.request);
    char lock[sizeof(database) + 6];
    snprintf(lock, sizeof(lock), "%s.lock", database);
    assert(!unlink(path) && !unlink(database) && !unlink(lock));
}
int main(void) {
    codecs();
    artifact_bound_and_integrity();
    invalid_inputs();
    typed_history();
    native_sources_and_atomicity();
    import_and_reopen();
    puts("content tests passed");
    return 0;
}
