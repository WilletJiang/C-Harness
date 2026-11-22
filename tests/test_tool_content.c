#include "core/content_internal.h"
#include "core/prompt.h"
#include "ah/jobs.h"
#include <assert.h>
#include <stdio.h>
#include <unistd.h>

static const char image[] =
    "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/"
    "x8AAwMCAO+aXioAAAAASUVORK5CYII=";
static const char call[] =
    "{\"status\":\"completed\",\"output\":[{\"type\":\"function_call\",\"name\":\"media\","
    "\"call_id\":\"media-call\",\"arguments\":\"{}\"}]}";
static const char done[] = "{\"status\":\"completed\",\"output\":[]}";
typedef struct {
    char *payload, *request;
    unsigned calls, executions;
    ah_tool_result_format format;
    bool error, unknown;
} fixture;

static void ok(ah_code code, ah_error *error) {
    if (code != AH_OK) {
        fprintf(stderr, "%s: %s\n", ah_code_name(code), error->message);
        abort();
    }
}
static int count(ah_engine *e, const char *sql) {
    sqlite3_stmt *s = NULL;
    assert(sqlite3_prepare_v2(e->db, sql, -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    int value = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return value;
}
static ah_code complete(void *context, const char *request, ah_delta_sink delta, void *dc,
                        ah_cancel_check cancel, void *cc, char **response, ah_error *error) {
    (void)delta;
    (void)dc;
    (void)cancel;
    (void)cc;
    (void)error;
    fixture *f = context;
    free(f->request);
    f->request = ah_strdup(request);
    *response = ah_strdup(f->calls++ ? done : call);
    return AH_OK;
}
static ah_code execute(void *context, const ah_tool_call *call_info, ah_tool_result *result,
                       ah_error *error) {
    (void)call_info;
    (void)error;
    fixture *f = context;
    f->executions++;
    result->json = ah_strdup(f->payload);
    result->format = f->format;
    result->is_error = f->error;
    result->outcome_unknown = f->unknown;
    return AH_OK;
}
static ah_engine *open_engine(fixture *f, const char *path, ah_effect effect, bool worker) {
    ah_config config;
    ah_config_init(&config);
    config.database_path = path;
    config.provider = (ah_provider){complete, f};
    config.max_model_tool_bytes = 1024;
    ah_error error = {0};
    ah_engine *e = NULL;
    ok(ah_open(&config, &e, &error), &error);
    ah_tool tool = {.name = "media",
                    .description = "Typed result",
                    .parameters_json =
                        "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
                    .effect = effect,
                    .execute = execute,
                    .context = f,
                    .parallel_safe = worker};
    ok(worker ? ah_register_background_tool(e, &tool, &error) : ah_register_tool(e, &tool, &error),
       &error);
    return e;
}
static char *payload(size_t repetitions) {
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *parts = yyjson_mut_arr(doc), *text = yyjson_mut_obj(doc),
                   *picture = yyjson_mut_obj(doc), *file = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, parts);
    char *message = malloc(repetitions * 3 + 1);
    for (size_t i = 0; i < repetitions; i++)
        memcpy(message + i * 3, "地", 3);
    message[repetitions * 3] = 0;
    yyjson_mut_obj_add_str(doc, text, "type", "input_text");
    yyjson_mut_obj_add_strcpy(doc, text, "text", message);
    yyjson_mut_obj_add_str(doc, picture, "type", "input_image");
    yyjson_mut_obj_add_str(doc, picture, "image_url", image);
    yyjson_mut_obj_add_str(doc, file, "type", "input_file");
    yyjson_mut_obj_add_str(doc, file, "file_data", "YQ==");
    yyjson_mut_obj_add_str(doc, file, "filename", "a.txt");
    yyjson_mut_arr_append(parts, text);
    yyjson_mut_arr_append(parts, picture);
    yyjson_mut_arr_append(parts, file);
    char *json = ah_mut_write(doc);
    ah_mut_doc_free(doc);
    free(message);
    assert(json);
    return json;
}
static yyjson_val *output(yyjson_doc *doc, size_t index) {
    yyjson_val *items = yyjson_obj_get(yyjson_doc_get_root(doc), "input"), *item;
    size_t i, n, found = 0;
    yyjson_arr_foreach(items, i, n, item) {
        const char *type = yyjson_get_str(yyjson_obj_get(item, "type"));
        if (type && !strcmp(type, "function_call_output") && found++ == index)
            return yyjson_obj_get(item, "output");
    }
    return NULL;
}
static void check_media(yyjson_val *parts, bool expanded) {
    assert(yyjson_is_arr(parts));
    unsigned images = 0, files = 0;
    size_t i, n;
    yyjson_val *part;
    yyjson_arr_foreach(parts, i, n, part) {
        const char *type = yyjson_get_str(yyjson_obj_get(part, "type"));
        assert(type);
        if (!strcmp(type, "input_image")) {
            images++;
            if (expanded)
                assert(!strcmp(yyjson_get_str(yyjson_obj_get(part, "image_url")), image));
            else
                assert(content_id_valid(yyjson_get_str(yyjson_obj_get(part, "artifact_id"))));
        } else if (!strcmp(type, "input_file")) {
            files++;
            if (expanded)
                assert(!strcmp(yyjson_get_str(yyjson_obj_get(part, "file_data")),
                               "data:text/plain;base64,YQ=="));
        }
    }
    assert(images == 1 && files == 1);
}
static void native_and_legacy(bool worker, bool native) {
    fixture f = {.payload = payload(2000),
                 .format = native ? AH_TOOL_RESULT_CONTENT : AH_TOOL_RESULT_JSON,
                 .error = true};
    ah_engine *e = open_engine(&f, ":memory:", AH_EFFECT_READ, worker);
    /* Reserve a fixed envelope around the prompt. Large raw text still exceeds
     * this budget until the receipt is reduced for the model. */
    e->config.max_context_bytes = sizeof(session_prompt) + 3584;
    assert(strlen(f.payload) > e->config.max_context_bytes);
    ah_error error = {0};
    char sid[AH_ID_CAP];
    ok(ah_session_create(e, "mock", "", sid, &error), &error);
    ok(ah_submit(e, sid, "task", "Inspect media", &error), &error);
    ok(ah_run(e, sid, &error), &error);
    assert(f.executions == 1 && f.calls == 2);
    yyjson_doc *request = yyjson_read(f.request, strlen(f.request), 0);
    yyjson_val *receipt = output(request, 0);
    assert(native ? yyjson_is_arr(receipt) : yyjson_is_str(receipt));
    if (native) {
        check_media(receipt, true);
        assert(count(e, "SELECT count(*) FROM artifacts") == 2);
        assert(count(e, "SELECT count(*) FROM operations WHERE result_format=1 AND "
                        "instr(result,'base64')=0") == 1);
        assert(count(e, "SELECT count(*) FROM events WHERE instr(json,'base64')>0") == 0);
        const char *meta = yyjson_get_str(yyjson_obj_get(yyjson_arr_get(receipt, 0), "text"));
        yyjson_doc *parsed = yyjson_read(meta, strlen(meta), 0);
        assert(yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(parsed), "is_error")));
        assert(yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(parsed), "truncated")));
        yyjson_doc_free(parsed);
        e->config.max_context_bytes = 100;
        core_session session;
        ok(core_load(e, sid, &session, &error), &error);
        char *wire = NULL;
        assert(core_request(e, &session, &wire, &error) == AH_LIMIT && !wire);
        core_session_free(&session);
    } else
        assert(count(e, "SELECT count(*) FROM artifacts") == 0);
    yyjson_doc_free(request);
    ah_close(e);
    free(f.payload);
    free(f.request);
}
static void invalid_results(void) {
    const char *bad[] = {
        "[]",
        "[null]",
        "[{\"type\":\"input_text\",\"text\":\"x\\u0000y\"}]",
        "[{\"type\":\"input_image\",\"image_url\":\"data:image/png;base64,YQ==\"}]",
        "[{\"type\":\"input_file\",\"file_data\":\"YQ==\",\"filename\":\"../a.txt\"}]",
        ("[{\"type\":\"input_image\",\"artifact_id\":\"00000000000000000000000000000000"
         "00000000000000000000000000000000\"}]")};
    for (unsigned mode = 0; mode < 4; mode++)
        for (size_t i = 0; i <= sizeof(bad) / sizeof(*bad); i++) {
            bool effect = mode & 1, worker = mode & 2;
            fixture f = {.payload = (char *)(i < sizeof(bad) / sizeof(*bad) ? bad[i] : "{}"),
                         .format = i < sizeof(bad) / sizeof(*bad) ? AH_TOOL_RESULT_CONTENT
                                                                  : (ah_tool_result_format)9};
            ah_engine *e =
                open_engine(&f, ":memory:", effect ? AH_EFFECT_EXTERNAL : AH_EFFECT_READ, worker);
            ah_error error = {0};
            char sid[AH_ID_CAP];
            ok(ah_session_create(e, "mock", "", sid, &error), &error);
            ok(ah_submit(e, sid, "bad", "Inspect", &error), &error);
            assert(ah_run(e, sid, &error) == (effect ? AH_UNCERTAIN : AH_OK));
            assert(count(e, "SELECT count(*) FROM artifacts") == 0);
            if (!effect)
                assert(
                    count(e,
                          "SELECT count(*) FROM operations WHERE is_error=1 AND result_format=0") ==
                    1);
            ah_close(e);
            free(f.request);
        }
}
static void restart_and_authority(void) {
    char directory[] = "/tmp/ah-tool-content-XXXXXX", path[256];
    assert(mkdtemp(directory));
    snprintf(path, sizeof(path), "%s/state.db", directory);
    fixture f = {.payload = payload(2), .format = AH_TOOL_RESULT_CONTENT, .calls = 1};
    ah_engine *e = open_engine(&f, path, AH_EFFECT_READ, false);
    ah_error error = {0}, execution = {0};
    char sid[AH_ID_CAP], foreign[AH_ID_CAP], forked[AH_ID_CAP], id[AH_ID_CAP];
    ok(ah_session_create(e, "mock", "", sid, &error), &error);
    ok(ah_submit(e, sid, "seed", "Inspect", &error), &error);
    core_session session;
    ok(core_load(e, sid, &session, &error), &error);
    const char *calls = "{\"status\":\"completed\",\"output\":[{\"type\":\"function_call\","
                        "\"name\":\"media\",\"call_id\":\"first\",\"arguments\":\"{}\"},"
                        "{\"type\":\"function_call\",\"name\":\"media\",\"call_id\":\"second\","
                        "\"arguments\":\"{}\"}]}";
    ok(core_accept_response(e, &session, calls, &error), &error);
    sqlite3_stmt *row = NULL;
    assert(sqlite3_prepare_v2(e->db, "SELECT id FROM operations WHERE call_id='second'", -1, &row,
                              NULL) == SQLITE_OK);
    assert(sqlite3_step(row) == SQLITE_ROW);
    snprintf(id, sizeof(id), "%s", sqlite3_column_text(row, 0));
    sqlite3_finalize(row);
    core_operation op;
    ok(core_get_operation(e, sid, id, &op, &error), &error);
    ah_tool_result result = {.json = ah_strdup(f.payload), .format = AH_TOOL_RESULT_CONTENT};
    size_t output_limit = e->config.max_output_bytes;
    e->config.max_output_bytes = 1;
    assert(core_normalize_result(e, sid, &op, &result, &error) == AH_INVALID);
    e->config.max_output_bytes = output_limit;
    ok(core_sql(e,
                "CREATE TRIGGER reject_capture BEFORE UPDATE ON operations "
                "WHEN NEW.status='received' BEGIN SELECT RAISE(ABORT,'injected'); END",
                &error),
       &error);
    assert(core_capture_operation(e, &session, &op, &result, AH_OK, &execution, AH_OK, &error) !=
           AH_OK);
    assert(count(e, "SELECT count(*) FROM artifacts") == 0);
    assert(count(e, "SELECT count(*) FROM artifact_refs") == 0);
    assert(count(e, "SELECT count(*) FROM operations WHERE result IS NOT NULL") == 0);
    ok(core_sql(e, "DROP TRIGGER reject_capture", &error), &error);
    /* A storage failure can canonicalize the caller-owned value; retry from the
     * actual callback result, never an uncommitted reference. */
    free(result.json);
    result.json = ah_strdup(f.payload);
    result.is_error = true;
    ok(core_capture_operation(e, &session, &op, &result, AH_OK, &execution, AH_LIMIT, &error),
       &error);
    free(result.json);
    core_operation_free(&op);
    ok(core_publish_ready(e, &session, &error), &error);
    assert(count(e, "SELECT count(*) FROM result_refs") == 0);
    core_session_free(&session);
    ah_close(e);
    e = open_engine(&f, path, AH_EFFECT_READ, false);
    ok(core_get_operation(e, sid, id, &op, &error), &error);
    assert(op.result_format == AH_TOOL_RESULT_CONTENT && !strcmp(op.status, "received_late"));
    core_operation_free(&op);
    ok(core_load(e, sid, &session, &error), &error);
    ok(core_next_operation(e, sid, &op, &error), &error);
    result = (ah_tool_result){.json = ah_strdup("{\"first\":true}")};
    ok(core_capture_operation(e, &session, &op, &result, AH_OK, &execution, AH_OK, &error), &error);
    free(result.json);
    core_operation_free(&op);
    ok(core_publish_ready(e, &session, &error), &error);
    char *wire = NULL;
    ok(core_request(e, &session, &wire, &error), &error);
    yyjson_doc *request = yyjson_read(wire, strlen(wire), 0);
    assert(yyjson_is_str(output(request, 0)));
    check_media(output(request, 1), true);
    yyjson_doc_free(request);
    free(wire);
    core_session_free(&session);
    ok(ah_run(e, sid, &error), &error);
    assert(f.executions == 0);
    assert(count(e, "SELECT count(*) FROM events WHERE type='tool.late_result'") == 1);
    ok(ah_fork(e, sid, forked, &error), &error);
    ok(ah_session_create(e, "mock", "", foreign, &error), &error);
    char arguments[128];
    snprintf(arguments, sizeof(arguments), "{\"operation_id\":\"%s\"}", id);
    ah_tool_call info = {.session_id = foreign, .arguments_json = arguments, .output_limit = 8192};
    result = (ah_tool_result){0};
    assert(core_read_result(e, &info, &result, &error) == AH_NOT_FOUND);
    info.session_id = forked;
    ok(core_read_result(e, &info, &result, &error), &error);
    assert(result.format == AH_TOOL_RESULT_CONTENT && result.is_error);
    yyjson_doc *parts = yyjson_read(result.json, strlen(result.json), 0);
    check_media(yyjson_doc_get_root(parts), false);
    yyjson_doc_free(parts);
    free(result.json);
    snprintf(arguments, sizeof(arguments), "{\"operation_id\":\"%s\",\"offset\":0,\"limit\":64}",
             id);
    result = (ah_tool_result){0};
    ok(core_read_result(e, &info, &result, &error), &error);
    assert(result.format == AH_TOOL_RESULT_JSON && strstr(result.json, "next_offset"));
    free(result.json);
    snprintf(arguments, sizeof(arguments), "{\"operation_id\":\"%s\"}", id);
    result = (ah_tool_result){0};
    ok(core_read_result(e, &info, &result, &error), &error);
    assert(result.format == AH_TOOL_RESULT_CONTENT);
    parts = yyjson_read(result.json, strlen(result.json), 0);
    const char *artifact = yyjson_get_str(
        yyjson_obj_get(yyjson_arr_get(yyjson_doc_get_root(parts), 1), "artifact_id"));
    assert(artifact && content_reference(e, foreign, artifact, true, &error) == AH_NOT_FOUND);
    yyjson_doc_free(parts);
    free(result.json);
    assert(ah_forget(e, sid, false, &error) == AH_BUSY);
    ok(ah_forget(e, forked, false, &error), &error);
    ok(ah_forget(e, sid, false, &error), &error);
    assert(count(e, "SELECT count(*) FROM artifacts") == 0);
    ah_close(e);
    free(f.payload);
    free(f.request);
    assert(!unlink(path));
    char lock[272];
    snprintf(lock, sizeof(lock), "%s.lock", path);
    assert(!unlink(lock));
    assert(!rmdir(directory));
}

static void uncertain_native_result(void) {
    fixture f = {.payload = payload(2), .format = AH_TOOL_RESULT_CONTENT, .unknown = true};
    ah_engine *e = open_engine(&f, ":memory:", AH_EFFECT_EXTERNAL, false);
    ah_error error = {0};
    char sid[AH_ID_CAP];
    ok(ah_session_create(e, "mock", "", sid, &error), &error);
    ok(ah_submit(e, sid, "task", "Inspect", &error), &error);
    assert(ah_run(e, sid, &error) == AH_UNCERTAIN);
    assert(count(e, "SELECT count(*) FROM artifacts") == 0);
    assert(count(e, "SELECT count(*) FROM events WHERE instr(json,'base64')>0") == 0);
    core_operation op;
    ok(core_next_operation(e, sid, &op, &error), &error);
    ok(core_sql(e, "UPDATE operations SET result_format=1 WHERE status='unknown'", &error), &error);
    ok(ah_resolve(e, sid, op.id, "{\"reconciled\":true}", false, &error), &error);
    core_operation_free(&op);
    ok(ah_run(e, sid, &error), &error);
    assert(f.executions == 1);
    assert(count(e, "SELECT count(*) FROM operations WHERE result_format=0 AND status='done'") ==
           1);
    yyjson_doc *request = yyjson_read(f.request, strlen(f.request), 0);
    assert(yyjson_is_str(output(request, 0)));
    yyjson_doc_free(request);
    ah_close(e);
    free(f.payload);
    free(f.request);
}
static char *stored_receipt(ah_engine *e) {
    sqlite3_stmt *row = NULL;
    assert(sqlite3_prepare_v2(e->db,
                              "SELECT json FROM items WHERE "
                              "json_extract(json,'$.type')='function_call_output'",
                              -1, &row, NULL) == SQLITE_OK);
    assert(sqlite3_step(row) == SQLITE_ROW);
    char *json = ah_strdup((const char *)sqlite3_column_text(row, 0));
    sqlite3_finalize(row);
    assert(json);
    return json;
}
static void migrate_legacy(void) {
    char directory[] = "/tmp/ah-tool-migration-XXXXXX", path[256], lock[272];
    assert(mkdtemp(directory));
    snprintf(path, sizeof(path), "%s/state.db", directory);
    fixture f = {.payload = "{\"content\":[{\"type\":\"input_text\",\"text\":\"business data\"}]}"};
    ah_engine *e = open_engine(&f, path, AH_EFFECT_READ, false);
    ah_error error = {0};
    char sid[AH_ID_CAP];
    ok(ah_session_create(e, "mock", "", sid, &error), &error);
    ok(ah_submit(e, sid, "task", "Inspect", &error), &error);
    ok(ah_run(e, sid, &error), &error);
    char *before = stored_receipt(e);
    ah_close(e);
    sqlite3 *db = NULL;
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    assert(sqlite3_exec(db,
                        "ALTER TABLE operations DROP COLUMN result_format;"
                        "PRAGMA user_version=6",
                        NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_close(db) == SQLITE_OK);
    e = open_engine(&f, path, AH_EFFECT_READ, false);
    assert(count(e, "PRAGMA user_version") == 8);
    assert(count(e, "SELECT count(*) FROM operations WHERE result_format=0") == 1);
    char *after = stored_receipt(e);
    assert(!strcmp(before, after));
    free(before);
    free(after);
    ah_close(e);
    free(f.request);
    assert(!unlink(path));
    snprintf(lock, sizeof(lock), "%s.lock", path);
    assert(!unlink(lock));
    assert(!rmdir(directory));
}
int main(void) {
    native_and_legacy(false, true);
    native_and_legacy(true, true);
    native_and_legacy(false, false);
    invalid_results();
    restart_and_authority();
    migrate_legacy();
    uncertain_native_result();
    puts("typed tool content tests passed");
    return 0;
}
