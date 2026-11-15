#include "agent_fixture.h"
#include "ah/jobs.h"
#include "ah/work.h"
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    agent_fixture fixture;
    char operation[AH_ID_CAP];
    atomic_uint calls;
    atomic_bool started, finished;
} durable_agent_fixture;

static const ah_work_config durable = {.version = 1, .replay = AH_REPLAY_SAFE};
static const char schema[] = "{\"type\":\"object\",\"additionalProperties\":false}";

static void capture_operation(agent_fixture *fixture, const ah_event *event) {
    durable_agent_fixture *state = (durable_agent_fixture *)fixture;
    if (event->operation_id && !strcmp(event->session_id, fixture->ids[0]) &&
        !strcmp(event->type, "tool.started"))
        memcpy(state->operation, event->operation_id, AH_ID_CAP);
}

static ah_code await_signal(void *context, const ah_tool_call *call, ah_tool_result *result,
                            ah_error *error) {
    (void)error;
    durable_agent_fixture *state = context;
    unsigned attempt = atomic_fetch_add(&state->calls, 1);
    if (!attempt) {
        assert(!strcmp(call->checkpoint_json, "{}"));
        result->continuation_json =
            strdup("{\"checkpoint\":{\"phase\":\"waiting\"},\"wait\":{\"signal\":\"ready\"}}");
    } else {
        assert(attempt == 1 && call->wake_json);
        yyjson_doc *doc = yyjson_read(call->wake_json, strlen(call->wake_json), 0);
        yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
        assert(yyjson_equals_str(yyjson_obj_get(root, "reason"), "signal"));
        assert(yyjson_is_true(yyjson_obj_get(yyjson_obj_get(root, "signal"), "value")));
        yyjson_doc_free(doc);
        result->json = strdup("{\"ready\":true}");
    }
    assert(result->json || result->continuation_json);
    return AH_OK;
}

static void register_durable(durable_agent_fixture *state, ah_execute_fn execute) {
    ah_tool tool = {.name = "durable_agent_tool",
                    .description = "Durable work owned by a fresh subagent",
                    .parameters_json = schema,
                    .effect = AH_EFFECT_READ,
                    .execute = execute,
                    .context = state,
                    .work = &durable};
    ah_error error = {0};
    af_ok(ah_register_background_tool(state->fixture.engine, &tool, &error), &error);
}

static char *waiting_reply(agent_fixture *fixture, agent_request *request) {
    if (strcmp(request->session, fixture->root)) {
        memcpy(fixture->ids[0], request->session, AH_ID_CAP);
        if (request->ordinal == 1) {
            assert(!strstr(request->request, "ROOT_PRIVATE"));
            return af_call("durable_agent_tool", "durable_child", "{}");
        }
        assert(request->ordinal == 2);
        yyjson_doc *receipt = af_receipt(request->request, "durable_child");
        assert(yyjson_is_true(yyjson_obj_get(af_result(receipt), "ready")));
        yyjson_doc_free(receipt);
        return af_text("DURABLE_CHILD_FINISHED");
    }
    if (request->ordinal == 1)
        return af_call("spawn", "durable_spawn",
                       "{\"name\":\"worker\",\"message\":\"WAIT_FOR_HOST_SIGNAL\"}");
    af_spawn_id(request->request, "durable_spawn", fixture->ids[0]);
    if (request->ordinal == 2) {
        const char *ids[] = {fixture->ids[0]};
        return af_join("durable_join", ids, 1, 10000);
    }
    assert(request->ordinal == 3);
    yyjson_doc *receipt = af_receipt(request->request, "durable_join");
    assert(yyjson_is_false(yyjson_obj_get(af_result(receipt), "timed_out")));
    yyjson_doc_free(receipt);
    assert(af_occurrences(request->request, "DURABLE_CHILD_FINISHED") == 1);
    return af_text("DURABLE_ROOT_FINISHED");
}

static void join_quiesces_and_resumes_after_reopen(void) {
    char database[] = "/tmp/ah-durable-agent-XXXXXX";
    int descriptor = mkstemp(database);
    assert(descriptor >= 0 && close(descriptor) == 0);
    durable_agent_fixture state = {
        .fixture = {.database = database, .reply = waiting_reply, .on_event = capture_operation}};
    atomic_init(&state.calls, 0);
    agent_fixture *fixture = &state.fixture;
    af_open(fixture, 2, 4, 2);
    register_durable(&state, await_signal);
    af_create(fixture, "ROOT_PRIVATE_DURABLE_WAIT");
    uint64_t start = ah_now_ms();
    af_run(fixture);
    assert(ah_now_ms() - start < 2000 && fixture->tick < 100);
    assert(atomic_load(&state.calls) == 1 && state.operation[0]);
    af_state(fixture, fixture->root, "waiting_agents");
    af_state(fixture, fixture->ids[0], "waiting_work");
    unsigned requests = fixture->count;
    af_run(fixture);
    assert(fixture->count == requests && atomic_load(&state.calls) == 1);
    af_reopen(fixture);
    register_durable(&state, await_signal);
    af_run(fixture);
    assert(fixture->count == requests && atomic_load(&state.calls) == 1);
    ah_error error = {0};
    ah_signal_result delivered;
    af_ok(ah_signal(fixture->engine, fixture->ids[0], state.operation, "ready", "true", &delivered,
                    &error),
          &error);
    assert(delivered == AH_SIGNAL_DELIVERED);
    af_reopen(fixture);
    register_durable(&state, await_signal);
    af_run(fixture);
    af_state(fixture, fixture->root, "completed");
    af_state(fixture, fixture->ids[0], "completed");
    assert(atomic_load(&state.calls) == 2 && af_session_calls(fixture, fixture->root) == 3);
    af_close(fixture);
    af_remove_database(database);
}

static ah_code late_continuation(void *context, const ah_tool_call *call, ah_tool_result *result,
                                 ah_error *error) {
    (void)error;
    durable_agent_fixture *state = context;
    atomic_fetch_add(&state->calls, 1);
    atomic_store(&state->started, true);
    struct timespec pause = {.tv_nsec = 1000000};
    while (!call->cancelled(call->cancel_context))
        nanosleep(&pause, NULL);
    result->continuation_json =
        strdup("{\"checkpoint\":{\"phase\":\"LATE_REVIVAL\"},\"wait\":{\"signal\":\"late\"}}");
    assert(result->continuation_json);
    atomic_store(&state->finished, true);
    return AH_OK;
}

static ah_code start_gate(void *context, const ah_tool_call *call, ah_tool_result *result,
                          ah_error *error) {
    (void)error;
    durable_agent_fixture *state = context;
    struct timespec pause = {.tv_nsec = 1000000};
    while (!atomic_load(&state->started) && !call->cancelled(call->cancel_context))
        nanosleep(&pause, NULL);
    assert(atomic_load(&state->started));
    result->json = strdup("{}");
    assert(result->json);
    return AH_OK;
}

static char *interrupt_reply(agent_fixture *fixture, agent_request *request) {
    if (!strcmp(request->session, fixture->root)) {
        if (request->ordinal == 1) {
            const char *names[] = {"spawn", "spawn"}, *ids[] = {"victim", "stopper"};
            const char *args[] = {"{\"name\":\"victim\",\"message\":\"TASK_DURABLE_VICTIM\"}",
                                  "{\"name\":\"stopper\",\"message\":\"TASK_DURABLE_STOPPER\"}"};
            return af_calls(2, names, ids, args);
        }
        af_spawn_id(request->request, "victim", fixture->ids[0]);
        af_spawn_id(request->request, "stopper", fixture->ids[1]);
        if (request->ordinal == 2) {
            const char *ids[] = {fixture->ids[0], fixture->ids[1]};
            return af_join("interrupt_join", ids, 2, 10000);
        }
        return af_text("DURABLE_INTERRUPT_CHECKED");
    }
    if (strstr(request->request, "TASK_DURABLE_VICTIM")) {
        memcpy(fixture->ids[0], request->session, AH_ID_CAP);
        if (request->ordinal == 1)
            return af_call("durable_agent_tool", "late_child", "{}");
        assert(request->ordinal == 2 && strstr(request->request, "EXPLICIT_FOLLOWUP"));
        yyjson_doc *receipt = af_receipt(request->request, "late_child");
        assert(receipt && yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(receipt), "is_error")));
        yyjson_doc_free(receipt);
        assert(!strstr(request->request, "LATE_REVIVAL"));
        return af_text("STOPPED_CHILD_FOLLOWUP");
    }
    assert(strstr(request->request, "TASK_DURABLE_STOPPER"));
    if (request->ordinal == 1)
        return af_call("start_gate", "gate", "{}");
    if (request->ordinal == 2) {
        char arguments[128];
        snprintf(arguments, sizeof(arguments), "{\"target\":\"%s\"}", fixture->ids[0]);
        return af_call("stop", "stop_sibling", arguments);
    }
    return af_text("STOPPER_FINISHED");
}

static void sibling_stop_fences_late_continuation(void) {
    durable_agent_fixture state = {
        .fixture = {.reply = interrupt_reply, .on_event = capture_operation}};
    atomic_init(&state.calls, 0);
    atomic_init(&state.started, false);
    atomic_init(&state.finished, false);
    agent_fixture *fixture = &state.fixture;
    af_open(fixture, 3, 4, 2);
    register_durable(&state, late_continuation);
    ah_tool gate = {.name = "start_gate",
                    .description = "Wait until the sibling callback is active",
                    .parameters_json = schema,
                    .effect = AH_EFFECT_READ,
                    .execute = start_gate,
                    .context = &state};
    ah_error error = {0};
    af_ok(ah_register_background_tool(fixture->engine, &gate, &error), &error);
    af_create(fixture, "INTERRUPT_DURABLE_SIBLING");
    af_run(fixture);
    assert(atomic_load(&state.finished) && atomic_load(&state.calls) == 1);
    af_state(fixture, fixture->ids[0], "stopped");
    char *json = NULL;
    af_ok(ah_work_json(fixture->engine, fixture->ids[0], state.operation, &json, &error), &error);
    yyjson_doc *snapshot = yyjson_read(json, strlen(json), 0);
    assert(snapshot &&
           yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(snapshot), "status"), "done"));
    assert(!strstr(json, "LATE_REVIVAL"));
    yyjson_doc_free(snapshot);
    free(json);
    ah_signal_result delivered;
    af_ok(ah_signal(fixture->engine, fixture->ids[0], state.operation, "late", "true", &delivered,
                    &error),
          &error);
    assert(delivered == AH_SIGNAL_CLOSED);
    af_run(fixture);
    assert(atomic_load(&state.calls) == 1);
    af_state(fixture, fixture->ids[0], "stopped");
    af_ok(ah_agent_send(fixture->engine, fixture->root, fixture->ids[0], "followup",
                        "EXPLICIT_FOLLOWUP", true, &error),
          &error);
    af_run(fixture);
    af_state(fixture, fixture->ids[0], "completed");
    assert(atomic_load(&state.calls) == 1);
    af_close(fixture);
}

void durable_agent_cases(void) {
    join_quiesces_and_resumes_after_reopen();
    sibling_stop_fences_late_continuation();
}
