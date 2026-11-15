#include "core_test.h"
#include "ah/agents.h"
#include "agents/scheduler.h"

/* The two provider adapters must preserve the same user/tool history across
 * pauses, cancellation and budgets. Keep the async request borrowed until release. */
typedef struct {
    fixture *fixture;
    const char *request;
    ah_delta_sink delta;
    void *delta_context;
    ah_cancel_check cancelled;
    void *cancel_context;
    bool ready;
} adapter;

static ah_code start(void *context, const char *request, ah_delta_sink delta, void *delta_context,
                     ah_cancel_check cancelled, void *cancel_context, void **handle,
                     ah_error *error) {
    (void)error;
    adapter *a = context;
    assert(!a->request);
    a->request = request;
    a->delta = delta;
    a->delta_context = delta_context;
    a->cancelled = cancelled;
    a->cancel_context = cancel_context;
    a->ready = false;
    *handle = a;
    return AH_OK;
}
static ah_code poll(void *context, unsigned timeout, ah_error *error) {
    (void)timeout;
    (void)error;
    ((adapter *)context)->ready = true;
    return AH_OK;
}
static ah_code result(void *context, void *handle, bool *finished, char **response,
                      ah_error *error) {
    adapter *a = context;
    assert(handle == a);
    *finished = a->ready;
    if (!a->ready)
        return AH_OK;
    return fixture_complete(a->fixture, a->request, a->delta, a->delta_context, a->cancelled,
                            a->cancel_context, response, error);
}
static void release(void *context, void *handle) {
    adapter *a = context;
    assert(handle == a && a->request);
    yyjson_doc *doc = yyjson_read(a->request, strlen(a->request), 0);
    assert(doc);
    yyjson_doc_free(doc);
    a->request = NULL;
}
static void enable(ah_engine *engine, adapter *a) {
    ah_agents_config config;
    ah_agents_config_init(&config);
    config.provider = (ah_async_provider){a, start, poll, result, release};
    ah_error error = {0};
    ok(ah_agents_enable(engine, &config, &error), &error);
}
static char *input(const fixture *f) {
    yyjson_doc *doc = yyjson_read(f->last_request, strlen(f->last_request), 0);
    assert(doc);
    char *json = yyjson_val_write(yyjson_obj_get(yyjson_doc_get_root(doc), "input"), 0, NULL);
    yyjson_doc_free(doc);
    assert(json);
    return json;
}

enum scenario { APPROVAL, ASK, STEER, CANCEL, UNKNOWN, MODEL_BUDGET, SCENARIOS };
static char *exercise(enum scenario scenario, bool async) {
    fixture f = {.answers = {call, done}};
    if (scenario == ASK)
        f.answers[0] = ask;
    ah_config config;
    ah_config_init(&config);
    if (scenario == MODEL_BUDGET)
        config.max_model_calls = 1;
    ah_engine *engine = open_engine(&f, ":memory:", &config);
    register_tool(engine, &f, scenario == UNKNOWN ? AH_EFFECT_EXTERNAL : AH_EFFECT_READ,
                  scenario == APPROVAL);
    adapter a = {.fixture = &f};
    if (async)
        enable(engine, &a);
    char session[AH_ID_CAP];
    create_input(engine, session);
    if (scenario == STEER) {
        f.steer = true;
        f.steering_session = session;
    }
    f.cancel_tool = scenario == CANCEL;
    f.unknown = scenario == UNKNOWN;
    ah_error error = {0};
    ah_code code = ah_run(engine, session, &error);
    if (scenario == APPROVAL || scenario == ASK) {
        ok(code, &error);
        state_is(engine, session, scenario == ASK ? "waiting_input" : "waiting_approval");
        assert(f.calls == 1 && !f.executions);
        char *operation = pending(engine, session);
        if (scenario == ASK)
            ok(ah_reply(engine, session, operation, "\"a\"", &error), &error);
        else
            ok(ah_decide(engine, session, operation, true, &error), &error);
        free(operation);
        ok(ah_run(engine, session, &error), &error);
    } else if (scenario == CANCEL) {
        assert(code == AH_CANCELLED);
        state_is(engine, session, "stopped");
        assert(f.calls == 1 && f.executions == 1);
        f.cancel_tool = false;
        ok(ah_resume(engine, session, &error), &error);
        ok(ah_run(engine, session, &error), &error);
    } else if (scenario == UNKNOWN) {
        assert(code == AH_UNCERTAIN);
        state_is(engine, session, "outcome_unknown");
        assert(ah_resume(engine, session, &error) == AH_UNCERTAIN);
        char *operation = pending(engine, session);
        ok(ah_resolve(engine, session, operation, "{\"confirmed\":true}", false, &error), &error);
        free(operation);
        ok(ah_run(engine, session, &error), &error);
    } else if (scenario == MODEL_BUDGET) {
        assert(code == AH_LIMIT);
        state_is(engine, session, "stopped");
        assert(f.calls == 1 && f.executions == 1);
        ok(ah_resume(engine, session, &error), &error);
        assert(ah_run(engine, session, &error) == AH_LIMIT);
        assert(f.calls == 1 && f.executions == 1);
    } else
        ok(code, &error);
    if (scenario != MODEL_BUDGET) {
        state_is(engine, session, "completed");
        assert(f.calls == 2 && f.executions == (scenario == ASK ? 0 : 1));
        ok(ah_run(engine, session, &error), &error);
        assert(f.calls == 2);
    }
    assert(!a.request);
    char *history = input(&f);
    ah_close(engine);
    free(f.last_request);
    return history;
}

static ah_code stale_result(void *context, void *handle, bool *finished, char **response,
                            ah_error *error) {
    (void)error;
    adapter *a = context;
    assert(handle == a && a->cancelled(a->cancel_context));
    /* Even a provider that ignores cancellation must not leak buffered deltas. */
    a->delta(a->delta_context, "stale", 5);
    *response = strdup("{\"status\":\"completed\",\"usage\":{\"input_tokens\":3,"
                       "\"output_tokens\":5},\"output\":[{\"type\":\"function_call\","
                       "\"name\":\"test_tool\",\"call_id\":\"stale-call\","
                       "\"arguments\":\"{\\\"value\\\":3}\"}]}");
    assert(*response);
    *finished = true;
    return AH_OK;
}

static void stale_model_result(bool generation_changed) {
    fixture f = {0};
    ah_engine *engine = open_engine(&f, ":memory:", NULL);
    adapter a = {.fixture = &f};
    enable(engine, &a);
    engine->agents.provider.result = stale_result;
    char session[AH_ID_CAP];
    create_input(engine, session);
    ah_error error = {0};
    ok(agents_register_root(engine, session, &error), &error);
    ok(core_state(engine, session, AH_RUNNING, &error), &error);
    agent_job job = {.engine = engine, .deadline = UINT64_MAX};
    ok(core_load(engine, session, &job.session, &error), &error);
    engine->deadline = UINT64_MAX;
    ok(agents_model_start(&job, &error), &error);
    if (generation_changed)
        job.generation++;
    else {
        job.session.state = AH_STOPPED;
        ok(core_state(engine, session, AH_STOPPED, &error), &error);
    }
    bool finished = false;
    ok(agents_model_finish(&job, &finished, &error), &error);
    assert(finished && !a.request && !job.request && !job.handle && job.cancelled);
    assert(f.provisional == 0 && f.executions == 0);
    sqlite3_stmt *row = NULL;
    ok(core_prepare(engine,
                    "SELECT (SELECT count(*) FROM operations),"
                    "(SELECT count(*) FROM agent_usage WHERE attempt=1 AND reported=1 "
                    "AND input_tokens=3 AND output_tokens=5)",
                    &row, &error),
       &error);
    assert(sqlite3_step(row) == SQLITE_ROW);
    assert(sqlite3_column_int(row, 0) == 0 && sqlite3_column_int(row, 1) == 1);
    sqlite3_finalize(row);
    core_session_free(&job.session);
    ah_close(engine);
}

int main(void) {
    stale_model_result(false);
    stale_model_result(true);
    for (enum scenario scenario = APPROVAL; scenario < SCENARIOS; scenario++) {
        char *sync = exercise(scenario, false), *async = exercise(scenario, true);
        assert(!strcmp(sync, async));
        free(sync);
        free(async);
    }
    return 0;
}
