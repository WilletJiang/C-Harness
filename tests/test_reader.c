#include "ah/observe.h"
#include "ah/work.h"
#include "yyjson.h"
#include <assert.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    ah_engine *engine;
    char session[AH_ID_CAP];
    atomic_bool entered, release, done;
    atomic_uint calls;
} writer;

typedef struct {
    ah_reader *reader;
    writer *writer;
    unsigned count;
    uint64_t last;
    bool wait_for_writer;
} observed;

static void ok(ah_code code, const ah_error *error) {
    if (code != AH_OK) {
        fprintf(stderr, "%s: %s\n", ah_code_name(code), error->message);
        abort();
    }
}

static void pause_ms(void) {
    struct timespec delay = {0, 1000000};
    nanosleep(&delay, NULL);
}

static void await(atomic_bool *condition) {
    uint64_t deadline = ah_now_ms() + 2000;
    while (!atomic_load(condition) && ah_now_ms() < deadline)
        pause_ms();
    assert(atomic_load(condition));
}

static ah_code provider(void *context, const char *request, ah_delta_sink delta,
                        void *delta_context, ah_cancel_check cancelled, void *cancel_context,
                        char **response, ah_error *error) {
    (void)context;
    (void)request;
    (void)delta;
    (void)delta_context;
    (void)cancelled;
    (void)cancel_context;
    (void)response;
    (void)error;
    abort();
}

static ah_code execute(void *context, const ah_tool_call *call, ah_tool_result *result,
                       ah_error *error) {
    (void)error;
    writer *state = context;
    atomic_fetch_add(&state->calls, 1);
    atomic_store(&state->entered, true);
    await(&state->release);
    assert(!call->cancelled(call->cancel_context));
    result->json = strdup("{\"finished\":true}");
    assert(result->json);
    return AH_OK;
}

static void *run_writer(void *context) {
    writer *state = context;
    ah_error error = {0};
    ok(ah_work_run(state->engine, state->session, &error), &error);
    atomic_store(&state->done, true);
    return NULL;
}

static void observe(void *context, const ah_event *event) {
    observed *state = context;
    assert(event->seq > state->last && event->session_id && event->type && event->json);
    yyjson_doc *doc = yyjson_read(event->json, strlen(event->json), 0);
    assert(doc);
    yyjson_doc_free(doc);
    state->last = event->seq;
    state->count++;
    if (!state->wait_for_writer)
        return;
    state->wait_for_writer = false;
    ah_error error = {0};
    char *json = NULL;
    assert(ah_reader_session_json(state->reader, state->writer->session, &json, &error) == AH_BUSY);
    assert(!json);
    atomic_store(&state->writer->release, true);
    /* A retained SELECT would make the writer's commit hit its 1 s busy timeout.
     * Finishing during this callback proves no read transaction remains held. */
    await(&state->writer->done);
}

static uint64_t state_is(ah_reader *reader, const char *sid, const char *wanted) {
    ah_error error = {0};
    char *json = NULL;
    ok(ah_reader_session_json(reader, sid, &json, &error), &error);
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    assert(doc && yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(doc), "state"), wanted));
    yyjson_val *cursor = yyjson_obj_get(yyjson_doc_get_root(doc), "cursor");
    assert(yyjson_is_uint(cursor) && yyjson_get_uint(cursor) > 0);
    uint64_t value = yyjson_get_uint(cursor);
    yyjson_doc_free(doc);
    ah_free(json);
    return value;
}

static uint64_t append_event(sqlite3 *db, const char *sid, size_t bytes) {
    char *json = malloc(bytes + 1);
    assert(json && bytes >= 2);
    memset(json, 'x', bytes);
    json[0] = json[bytes - 1] = '"';
    json[bytes] = 0;
    sqlite3_stmt *row = NULL;
    assert(sqlite3_prepare_v2(db,
                              "INSERT INTO events(session_id,type,json) VALUES(?,'reader.test',?)",
                              -1, &row, NULL) == SQLITE_OK);
    sqlite3_bind_text(row, 1, sid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(row, 2, json, -1, SQLITE_TRANSIENT);
    assert(sqlite3_step(row) == SQLITE_DONE);
    sqlite3_finalize(row);
    free(json);
    return (uint64_t)sqlite3_last_insert_rowid(db);
}

static void batch_bounds(ah_reader *reader, sqlite3 *db, const char *sid, uint64_t cursor) {
    ah_error error = {0};
    observed seen = {.last = cursor};
    uint64_t first = append_event(db, sid, 2 * 1024 * 1024);
    uint64_t second = append_event(db, sid, 2 * 1024 * 1024);
    uint64_t last = 0;
    ok(ah_reader_events(reader, sid, cursor, 256, observe, &seen, &last, &error), &error);
    assert(seen.count == 1 && last == first);
    ok(ah_reader_events(reader, sid, last, 256, observe, &seen, &last, &error), &error);
    assert(seen.count == 2 && last == second);
    uint64_t small = append_event(db, sid, 2);
    uint64_t oversized = append_event(db, sid, 4 * 1024 * 1024 + 1);
    ok(ah_reader_events(reader, sid, last, 256, observe, &seen, &last, &error), &error);
    assert(seen.count == 3 && last == small);
    assert(ah_reader_events(reader, sid, last, 256, observe, &seen, &last, &error) == AH_LIMIT);
    assert(seen.count == 3 && last == small);
    assert(ah_reader_events(reader, sid, last, 257, observe, &seen, &last, &error) == AH_INVALID);
    assert(last == small);
    assert(ah_reader_events(reader, sid, last, 0, observe, &seen, &last, &error) == AH_INVALID);
    ok(ah_reader_events(reader, sid, UINT64_MAX, 1, observe, &seen, &last, &error), &error);
    assert(last == UINT64_MAX && seen.count == 3);

    observed recent = {0};
    assert(ah_reader_recent_events(reader, sid, 0, 3, observe, &recent, &last, &error) ==
           AH_LIMIT);
    assert(last == 0 && recent.count == 0);
    ok(ah_reader_recent_events(reader, sid, oversized, 2, observe, &recent, &last, &error),
       &error);
    assert(last == second && recent.count == 2 && recent.last == small);
    recent = (observed){0};
    ok(ah_reader_recent_events(reader, sid, last, 1, observe, &recent, &last, &error), &error);
    assert(last == first && recent.count == 1 && recent.last == first);
    ok(ah_reader_recent_events(reader, sid, 1, 1, observe, &recent, &last, &error), &error);
    assert(last == 0 && recent.count == 1);

    ok(ah_reader_set_busy_timeout(reader, 0, &error), &error);
    assert(sqlite3_exec(db, "BEGIN EXCLUSIVE", NULL, NULL, NULL) == SQLITE_OK);
    char *json = NULL;
    assert(ah_reader_session_json(reader, sid, &json, &error) == AH_BUSY && !json);
    last = second;
    assert(ah_reader_events(reader, sid, last, 1, observe, &seen, &last, &error) == AH_BUSY);
    assert(last == second && seen.count == 3);
    assert(sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
}

int main(void) {
    char path[] = "/tmp/ah-reader-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    assert(unlink(path) == 0);
    ah_error error = {0};
    ah_reader *reader = NULL;
    assert(ah_reader_open(path, &reader, &error) == AH_IO && !reader);
    assert(access(path, F_OK) != 0);
    assert(ah_reader_open(":memory:", &reader, &error) == AH_INVALID);

    writer state = {0};
    ah_config config;
    ah_config_init(&config);
    config.database_path = path;
    config.provider.complete = provider;
    ok(ah_open(&config, &state.engine, &error), &error);
    ah_tool tool = {.name = "blocked",
                    .description = "Reader concurrency test",
                    .parameters_json = "{\"type\":\"object\"}",
                    .execute = execute,
                    .context = &state};
    ok(ah_register_tool(state.engine, &tool, &error), &error);
    ok(ah_session_create(state.engine, "mock", "", state.session, &error), &error);
    char operation[AH_ID_CAP];
    ok(ah_work_start(state.engine, state.session, "root", "blocked", "{}", operation, &error),
       &error);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, run_writer, &state) == 0);
    await(&state.entered);
    ok(ah_reader_open(path, &reader, &error), &error);
    uint64_t before = state_is(reader, state.session, "running");
    observed seen = {.reader = reader, .writer = &state, .wait_for_writer = true};
    uint64_t cursor = 0;
    ok(ah_reader_events(reader, state.session, 0, 1, observe, &seen, &cursor, &error), &error);
    assert(cursor && seen.count == 1);
    assert(pthread_join(thread, NULL) == 0 && atomic_load(&state.calls) == 1);
    uint64_t completed = state_is(reader, state.session, "completed");
    assert(completed > before);
    observed snapshot = {.last = completed};
    uint64_t next = completed;
    ok(ah_reader_events(reader, state.session, completed, 256, observe, &snapshot, &next, &error),
       &error);
    assert(snapshot.count == 0 && next == completed);
    ah_reader_close(reader);
    ok(ah_reader_open(path, &reader, &error), &error);
    seen.reader = reader;
    ok(ah_reader_events(reader, NULL, cursor, 256, observe, &seen, &cursor, &error), &error);
    assert(seen.count > 1 && cursor == seen.last);
    unsigned count = seen.count;
    ok(ah_reader_events(reader, state.session, cursor, 256, observe, &seen, &cursor, &error),
       &error);
    assert(seen.count == count);
    char *json = NULL;
    assert(ah_reader_session_json(reader, "missing", &json, &error) == AH_NOT_FOUND && !json);
    ah_close(state.engine);

    sqlite3 *db = NULL;
    assert(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK);
    batch_bounds(reader, db, state.session, cursor);
    ah_reader_close(reader);
    assert(sqlite3_exec(db, "PRAGMA user_version=7", NULL, NULL, NULL) == SQLITE_OK);
    assert(ah_reader_open(path, &reader, &error) == AH_PROTOCOL && !reader);
    sqlite3_stmt *version = NULL;
    assert(sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &version, NULL) == SQLITE_OK);
    assert(sqlite3_step(version) == SQLITE_ROW && sqlite3_column_int(version, 0) == 7);
    sqlite3_finalize(version);
    char interrupted[160];
    snprintf(interrupted, sizeof(interrupted),
             "PRAGMA user_version=8; UPDATE sessions SET state=%d; "
             "UPDATE operations SET status='executing'",
             AH_RUNNING);
    assert(sqlite3_exec(db, interrupted, NULL, NULL, NULL) == SQLITE_OK);
    ok(ah_reader_open(path, &reader, &error), &error);
    state_is(reader, state.session, "running");
    assert(atomic_load(&state.calls) == 1);
    ah_reader_close(reader);
    sqlite3_close(db);
    assert(unlink(path) == 0);
    char lock[80];
    snprintf(lock, sizeof(lock), "%s.lock", path);
    assert(unlink(lock) == 0);
    puts("read-only snapshots, cursor batches and writer isolation passed");
    return 0;
}
