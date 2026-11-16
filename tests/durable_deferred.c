#include "durable_fixture.h"
#include "ah/jobs.h"
#include "ah/work.h"
#include <sqlite3.h>
#include <sys/wait.h>
#include <time.h>

typedef struct {
    char parent[AH_ID_CAP];
    bool external, replay_never;
    unsigned parent_starts;
    atomic_bool slow_started;
} deferred_fixture;

static ah_code no_model(void *context, const char *request, ah_delta_sink delta,
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
    assert(!"deferred host work must not invoke a model");
    return AH_PROTOCOL;
}

static void crash_after_parent_receipt(void *context, const ah_event *event) {
    deferred_fixture *fixture = context;
    if (event->operation_id && !strcmp(event->operation_id, fixture->parent) &&
        !strcmp(event->type, "tool.started") && ++fixture->parent_starts == 2 &&
        fixture->replay_never)
        _exit(77);
    if (event->operation_id && !strcmp(event->operation_id, fixture->parent) &&
        !strcmp(event->type, "tool.received") && !fixture->replay_never)
        _exit(77);
}

static ah_engine *open_deferred(const char *path, deferred_fixture *fixture) {
    ah_config config;
    ah_config_init(&config);
    config.database_path = path;
    config.provider.complete = no_model;
    config.run_timeout_ms = 3000;
    config.tool_timeout_ms = 2000;
    if (fixture) {
        config.observer = crash_after_parent_receipt;
        config.observer_context = fixture;
    }
    ah_engine *engine = NULL;
    ah_error error = {0};
    df_ok(ah_open(&config, &engine, &error), &error);
    return engine;
}

static ah_code deferred_parent(void *context, const ah_tool_call *call, ah_tool_result *result,
                               ah_error *error) {
    (void)error;
    deferred_fixture *fixture = context;
    if (strcmp(call->checkpoint_json, "{}")) {
        assert(atomic_load(&fixture->slow_started));
        result->json = strdup("{\"parent\":true}");
    } else if (fixture->external) {
        struct timespec wall;
        assert(clock_gettime(CLOCK_REALTIME, &wall) == 0);
        uint64_t until = (uint64_t)wall.tv_sec * 1000 + (uint64_t)wall.tv_nsec / 1000000 + 50;
        char continuation[512];
        snprintf(continuation, sizeof(continuation),
                 "{\"checkpoint\":{\"phase\":\"children\"},\"calls\":["
                 "{\"key\":\"slow\",\"tool\":\"slow\",\"arguments\":{}}],"
                 "\"wait\":{\"until_ms\":%llu}}",
                 (unsigned long long)until);
        result->continuation_json = strdup(continuation);
    } else
        result->continuation_json =
            strdup("{\"checkpoint\":{\"phase\":\"children\"},\"calls\":["
                   "{\"key\":\"fast\",\"tool\":\"fast\",\"arguments\":{}},"
                   "{\"key\":\"slow\",\"tool\":\"slow\",\"arguments\":{}}],"
                   "\"wait\":{\"calls\":[\"fast\",\"slow\"],\"policy\":\"any\"}}");
    assert(result->json || result->continuation_json);
    return AH_OK;
}

static ah_code fast_child(void *context, const ah_tool_call *call, ah_tool_result *result,
                          ah_error *error) {
    (void)error;
    deferred_fixture *fixture = context;
    while (!atomic_load(&fixture->slow_started) && !call->cancelled(call->cancel_context))
        df_sleep(1);
    assert(atomic_load(&fixture->slow_started));
    result->json = strdup("{}");
    assert(result->json);
    return AH_OK;
}

static ah_code slow_child(void *context, const ah_tool_call *call, ah_tool_result *result,
                          ah_error *error) {
    (void)error;
    deferred_fixture *fixture = context;
    atomic_store(&fixture->slow_started, true);
    while (!call->cancelled(call->cancel_context))
        df_sleep(1);
    result->json = strdup("{}");
    assert(result->json);
    result->outcome_unknown = fixture->external;
    return AH_OK;
}

static void crash_deferred(const char *database, bool external, bool replay_never) {
    deferred_fixture fixture = {.external = external, .replay_never = replay_never};
    atomic_init(&fixture.slow_started, false);
    ah_engine *engine = open_deferred(database, &fixture);
    const ah_work_config work = {.version = 1,
                                 .replay = replay_never ? AH_REPLAY_NEVER : AH_REPLAY_SAFE};
    const ah_work_config safe = {.version = 1, .replay = AH_REPLAY_SAFE};
    ah_tool tool = {.name = "parent",
                    .description = "Deferred parent",
                    .parameters_json = "{\"type\":\"object\"}",
                    .effect = AH_EFFECT_READ,
                    .execute = deferred_parent,
                    .context = &fixture,
                    .work = &work};
    ah_error error = {0};
    df_ok(ah_register_background_tool(engine, &tool, &error), &error);
    tool.name = "fast";
    tool.execute = fast_child;
    tool.work = NULL;
    df_ok(ah_register_background_tool(engine, &tool, &error), &error);
    tool.name = "slow";
    tool.execute = slow_child;
    tool.effect = external ? AH_EFFECT_EXTERNAL : AH_EFFECT_READ;
    tool.work = external ? NULL : &safe;
    df_ok(ah_register_background_tool(engine, &tool, &error), &error);
    char session[AH_ID_CAP];
    df_ok(ah_session_create(engine, "unused", "", session, &error), &error);
    df_ok(ah_work_start(engine, session, "root", "parent", "{}", fixture.parent, &error), &error);
    ah_work_run(engine, session, &error);
    assert(!"observer must crash after committing a deferred parent receipt");
    _exit(2);
}

static void operation_status(ah_engine *engine, const char *session, const char *operation,
                             const char *status) {
    ah_error error = {0};
    char *json = NULL;
    df_ok(ah_work_json(engine, session, operation, &json, &error), &error);
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    assert(doc && yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(doc), "status"), status));
    yyjson_doc_free(doc);
    free(json);
}

typedef struct {
    const char *parent;
    unsigned completed;
} deferred_events;
static void count_parent(void *context, const ah_event *event) {
    deferred_events *count = context;
    count->completed += event->operation_id && !strcmp(event->operation_id, count->parent) &&
                        !strcmp(event->type, "tool.completed");
}

static void deferred_recovery(bool external, bool replay_never) {
    char database[64];
    df_database(database);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        alarm(10);
        crash_deferred(database, external, replay_never);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 77);
    sqlite3 *db = NULL;
    assert(sqlite3_open(database, &db) == SQLITE_OK);
    sqlite3_stmt *row = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT session_id,id,status FROM operations WHERE name='parent'",
                              -1, &row, NULL) == SQLITE_OK);
    assert(sqlite3_step(row) == SQLITE_ROW);
    char session[AH_ID_CAP], parent[AH_ID_CAP], slow[AH_ID_CAP];
    snprintf(session, sizeof(session), "%s", sqlite3_column_text(row, 0));
    snprintf(parent, sizeof(parent), "%s", sqlite3_column_text(row, 1));
    assert(!strcmp((const char *)sqlite3_column_text(row, 2),
                   replay_never ? "executing" : "received"));
    sqlite3_finalize(row);
    assert(sqlite3_prepare_v2(db, "SELECT id,status FROM operations WHERE name='slow'", -1, &row,
                              NULL) == SQLITE_OK);
    assert(sqlite3_step(row) == SQLITE_ROW);
    snprintf(slow, sizeof(slow), "%s", sqlite3_column_text(row, 0));
    assert(!strcmp((const char *)sqlite3_column_text(row, 1), external ? "unknown" : "executing"));
    sqlite3_finalize(row);
    assert(sqlite3_close(db) == SQLITE_OK);
    ah_engine *engine = open_deferred(database, NULL);
    operation_status(engine, session, parent, "received");
    operation_status(engine, session, slow, external ? "unknown" : "work_ready");
    ah_error error = {0};
    if (external) {
        assert(ah_work_run(engine, session, &error) == AH_UNCERTAIN);
        operation_status(engine, session, parent, "received");
        df_ok(ah_resolve(engine, session, slow, "{\"reconciled\":true}", false, &error), &error);
    } else
        df_ok(ah_resume(engine, session, &error), &error);
    df_ok(ah_work_run(engine, session, &error), &error);
    operation_status(engine, session, parent, "done");
    operation_status(engine, session, slow, "done");
    df_ok(ah_work_run(engine, session, &error), &error);
    deferred_events count = {.parent = parent};
    df_ok(ah_events(engine, session, 0, count_parent, &count, &error), &error);
    assert(count.completed == 1);
    ah_close(engine);
    df_remove_database(database);
}

void df_deferred_cases(void) {
    deferred_recovery(false, false);
    deferred_recovery(true, false);
    deferred_recovery(false, true);
}
