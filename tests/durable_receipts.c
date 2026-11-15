#include "durable_fixture.h"
#include "core/work.h"
#include "ah/jobs.h"

typedef struct {
    durable_fixture fixture;
    pthread_key_t completion;
    atomic_uint exited, children, parents;
} receipt_fixture;

static void worker_exited(void *context) {
    receipt_fixture *state = context;
    atomic_fetch_add(&state->exited, 1);
}

static ah_code child(void *context, const ah_tool_call *call, ah_tool_result *result,
                     ah_error *error) {
    (void)error;
    receipt_fixture *state = context;
    assert(pthread_setspecific(state->completion, state) == 0);
    atomic_fetch_add(&state->children, 1);
    result->json = strdup("{\"value\":7}");
    assert(result->json);
    result->outcome_unknown = strstr(call->arguments_json, "true") != NULL;
    return AH_OK;
}

static ah_code parent(void *context, const ah_tool_call *call, ah_tool_result *result,
                      ah_error *error) {
    (void)call;
    (void)error;
    receipt_fixture *state = context;
    atomic_fetch_add(&state->parents, 1);
    result->json = strdup("{\"joined\":true}");
    assert(result->json);
    return AH_OK;
}

static void register_tools(receipt_fixture *state) {
    ah_error error = {0};
    ah_work_config work = {.version = 1, .replay = AH_REPLAY_SAFE};
    ah_tool root = {.name = "parent",
                    .description = "Owner of captured reads",
                    .parameters_json = "{\"type\":\"object\"}",
                    .execute = parent,
                    .context = state,
                    .work = &work};
    ah_tool read = {.name = "child",
                    .description = "Hidden concurrent read",
                    .parameters_json = "{\"type\":\"object\"}",
                    .execute = child,
                    .context = state,
                    .parallel_safe = true};
    df_ok(ah_register_tool(state->fixture.engine, &root, &error), &error);
    df_ok(ah_register_tool(state->fixture.engine, &read, &error), &error);
}

static void child_id(ah_engine *engine, const char *root, const char *key, char id[AH_ID_CAP]) {
    ah_error error = {0};
    sqlite3_stmt *row = NULL;
    df_ok(core_prepare(engine, "SELECT operation_id FROM work_calls WHERE parent_id=? AND key=?",
                       &row, &error),
          &error);
    sqlite3_bind_text(row, 1, root, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(row, 2, key, -1, SQLITE_TRANSIENT);
    assert(sqlite3_step(row) == SQLITE_ROW);
    snprintf(id, AH_ID_CAP, "%s", (const char *)sqlite3_column_text(row, 0));
    sqlite3_finalize(row);
}

static void captured(ah_engine *engine, const char *session, const char *id) {
    ah_error error = {0};
    core_operation operation;
    df_ok(core_get_operation(engine, session, id, &operation, &error), &error);
    assert(!strcmp(operation.status, "done") && !operation.is_error);
    assert(operation.result && !strcmp(operation.result, "{\"value\":7}"));
    core_operation_free(&operation);
}

static ah_code returned_continuation(void *context, const ah_tool_call *call,
                                     ah_tool_result *result, ah_error *error) {
    (void)call;
    (void)error;
    receipt_fixture *state = context;
    assert(pthread_setspecific(state->completion, state) == 0);
    atomic_fetch_add(&state->children, 1);
    result->continuation_json = strdup("{\"checkpoint\":{},\"wait\":{\"signal\":\"later\"}}");
    assert(result->continuation_json);
    return AH_OK;
}

static void cancel_before_capture(void) {
    receipt_fixture state = {0};
    assert(pthread_key_create(&state.completion, worker_exited) == 0);
    durable_fixture *f = &state.fixture;
    df_open(f, ":memory:", 1);
    ah_work_config work = {.version = 1, .replay = AH_REPLAY_SAFE};
    ah_tool tool = {.name = "park",
                    .description = "Returned but uncaptured continuation",
                    .parameters_json = "{\"type\":\"object\"}",
                    .execute = returned_continuation,
                    .context = &state,
                    .work = &work};
    ah_error error = {0};
    df_ok(ah_register_background_tool(f->engine, &tool, &error), &error);
    char sid[AH_ID_CAP], id[AH_ID_CAP];
    df_ok(ah_session_create(f->engine, "mock", "", sid, &error), &error);
    df_ok(ah_work_start(f->engine, sid, "cancel", "park", "{}", id, &error), &error);
    core_session session;
    core_operation operation;
    df_ok(core_load(f->engine, sid, &session, &error), &error);
    df_ok(core_get_operation(f->engine, sid, id, &operation, &error), &error);
    df_ok(core_tool_intents(f->engine, &session, &operation, 1, &error), &error);
    df_ok(core_dispatch_tool(f->engine, &session, &operation, true, false, &error), &error);
    core_operation_free(&operation);
    uint64_t until = ah_now_ms() + 2000;
    while (!atomic_load(&state.exited) && ah_now_ms() < until)
        df_sleep(1);
    assert(atomic_load(&state.exited) == 1);
    df_ok(ah_cancel_job(f->engine, id, &error), &error);
    assert(core_jobs_finish(f->engine, &session, &error) == AH_CANCELLED);
    df_ok(core_get_operation(f->engine, sid, id, &operation, &error), &error);
    assert(!strcmp(operation.status, "done") && operation.is_error);
    core_operation_free(&operation);
    core_session_free(&session);
    ah_signal_result delivery;
    df_ok(ah_signal(f->engine, sid, id, "later", "true", &delivery, &error), &error);
    assert(delivery == AH_SIGNAL_CLOSED);
    df_ok(ah_work_run(f->engine, sid, &error), &error);
    assert(atomic_load(&state.children) == 1 && !df_events(f, sid, "work.parked"));
    ah_close(f->engine);
    assert(pthread_key_delete(state.completion) == 0);
}

void durable_receipt_cases(void) {
    char database[64], session_id[AH_ID_CAP], root_id[AH_ID_CAP], ids[2][AH_ID_CAP];
    df_database(database);
    receipt_fixture state = {0};
    assert(pthread_key_create(&state.completion, worker_exited) == 0);
    durable_fixture *f = &state.fixture;
    df_open(f, database, 2);
    register_tools(&state);
    ah_error error = {0};
    df_ok(ah_session_create(f->engine, "mock", "", session_id, &error), &error);
    df_ok(ah_work_start(f->engine, session_id, "root", "parent", "{}", root_id, &error), &error);
    core_session session;
    core_operation root;
    df_ok(core_load(f->engine, session_id, &session, &error), &error);
    df_ok(core_get_operation(f->engine, session_id, root_id, &root, &error), &error);
    df_ok(core_tool_intents(f->engine, &session, &root, 1, &error), &error);
    uint64_t after = 0;
    df_ok(core_begin(f->engine, &after, &error), &error);
    df_ok(core_work_apply(f->engine, session_id, root_id, root.generation,
                          "{\"checkpoint\":{},\"calls\":["
                          "{\"key\":\"unknown\",\"tool\":\"child\","
                          "\"arguments\":{\"unknown\":true}},"
                          "{\"key\":\"good\",\"tool\":\"child\","
                          "\"arguments\":{\"unknown\":false}}],"
                          "\"wait\":{\"calls\":[\"unknown\",\"good\"]}}",
                          &error),
          &error);
    df_ok(core_commit(f->engine, after, &error), &error);
    core_operation_free(&root);
    child_id(f->engine, root_id, "unknown", ids[0]);
    child_id(f->engine, root_id, "good", ids[1]);
    core_operation children[2];
    for (unsigned i = 0; i < 2; i++)
        df_ok(core_get_operation(f->engine, session_id, ids[i], &children[i], &error), &error);
    df_ok(core_jobs_init(f->engine, &error), &error);
    df_ok(core_tool_intents(f->engine, &session, children, 2, &error), &error);
    for (unsigned i = 0; i < 2; i++) {
        assert(children[i].hidden);
        df_ok(core_dispatch_tool(f->engine, &session, &children[i], true, false, &error), &error);
        core_operation_free(&children[i]);
    }
    /* TLS destructors run after the worker publishes its finished flag. Both
     * results therefore belong to this exact drain batch, without timing luck. */
    uint64_t deadline = ah_now_ms() + 2000;
    while (atomic_load(&state.exited) < 2 && ah_now_ms() < deadline)
        df_sleep(1);
    assert(atomic_load(&state.exited) == 2);
    assert(core_jobs_finish(f->engine, &session, &error) == AH_UNCERTAIN);
    assert(!core_jobs_running(f->engine, session_id));
    captured(f->engine, session_id, ids[1]);
    core_session_free(&session);
    ah_close(f->engine);

    df_open(f, database, 2);
    register_tools(&state);
    captured(f->engine, session_id, ids[1]);
    assert(ah_work_run(f->engine, session_id, &error) == AH_UNCERTAIN);
    df_ok(ah_resolve(f->engine, session_id, ids[0], "{\"value\":7}", false, &error), &error);
    df_ok(ah_work_run(f->engine, session_id, &error), &error);
    assert(atomic_load(&state.children) == 2 && atomic_load(&state.parents) == 1 && !f->models);
    captured(f->engine, session_id, ids[1]);
    df_state(f, session_id, AH_COMPLETED);
    ah_close(f->engine);
    assert(pthread_key_delete(state.completion) == 0);
    df_remove_database(database);
    cancel_before_capture();
}
