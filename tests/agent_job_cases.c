#include "agent_fixture.h"
#include "ah/jobs.h"
#include "core/core.h"
#include <stdatomic.h>
#include <time.h>

typedef struct {
    agent_fixture fixture;
    atomic_bool started, finished;
    unsigned calls;
    bool native;
} interrupt_fixture;

static ah_code waiting_effect(void *context, const ah_tool_call *call, ah_tool_result *result,
                              ah_error *error) {
    (void)error;
    interrupt_fixture *state = context;
    state->calls++;
    atomic_store(&state->started, true);
    struct timespec pause = {.tv_nsec = 1000000};
    while (!call->cancelled(call->cancel_context))
        nanosleep(&pause, NULL);
    result->json = strdup(state->native ? "[{\"type\":\"input_image\",\"image_url\":"
                                          "\"data:image/png;base64,AAECAwQ=\"}]"
                                        : "{\"interrupted\":true}");
    result->format = state->native ? AH_TOOL_RESULT_CONTENT : AH_TOOL_RESULT_JSON;
    result->outcome_unknown = true;
    atomic_store(&state->finished, true);
    return AH_OK;
}

static ah_code started_gate(void *context, const ah_tool_call *call, ah_tool_result *result,
                            ah_error *error) {
    (void)error;
    interrupt_fixture *state = context;
    struct timespec pause = {.tv_nsec = 1000000};
    while (!atomic_load(&state->started) && !call->cancelled(call->cancel_context))
        nanosleep(&pause, NULL);
    result->json = strdup("{}");
    return AH_OK;
}

static char *interrupt_reply(agent_fixture *fixture, agent_request *request) {
    if (strcmp(request->session, fixture->root)) {
        assert(request->ordinal == 1);
        return af_call("waiting_effect", "slow", "{}");
    }
    if (request->ordinal == 1)
        return af_call("spawn", "child", "{\"name\":\"worker\",\"message\":\"WAIT\"}");
    af_spawn_id(request->request, "child", fixture->ids[0]);
    if (request->ordinal == 2)
        return af_call("started_gate", "gate", "{}");
    if (request->ordinal == 3) {
        char args[128];
        snprintf(args, sizeof(args), "{\"target\":\"%s\"}", fixture->ids[0]);
        return af_call("stop", "stop", args);
    }
    return af_text("ROOT_DONE");
}

static void interrupt_case(bool native) {
    interrupt_fixture state = {.fixture = {.reply = interrupt_reply}, .native = native};
    atomic_init(&state.started, false);
    atomic_init(&state.finished, false);
    agent_fixture *fixture = &state.fixture;
    af_open(fixture, 2, 4, 2);
    ah_tool tool = {.name = "waiting_effect",
                    .description = "Interruptible effect",
                    .parameters_json = "{\"type\":\"object\",\"additionalProperties\":false}",
                    .effect = AH_EFFECT_EXTERNAL,
                    .execute = waiting_effect,
                    .context = &state};
    ah_error error = {0};
    af_ok(ah_register_background_tool(fixture->engine, &tool, &error), &error);
    tool.name = "started_gate";
    tool.effect = AH_EFFECT_READ;
    tool.execute = started_gate;
    af_ok(ah_register_background_tool(fixture->engine, &tool, &error), &error);
    af_create(fixture, "INTERRUPT_RUNNING_JOB");
    af_run(fixture);
    assert(atomic_load(&state.finished) && state.calls == 1);
    assert(!core_jobs_running(fixture->engine, NULL));
    af_state(fixture, fixture->ids[0], "outcome_unknown");
    core_operation operation;
    af_ok(core_next_operation(fixture->engine, fixture->ids[0], &operation, &error), &error);
    assert(!strcmp(operation.status, "unknown"));
    sqlite3_stmt *stmt = NULL;
    af_ok(
        core_prepare(fixture->engine,
                     "SELECT count(*) FROM events WHERE operation_id=? AND type='tool.late_result'",
                     &stmt, &error),
        &error);
    sqlite3_bind_text(stmt, 1, operation.id, -1, SQLITE_TRANSIENT);
    assert(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1);
    sqlite3_finalize(stmt);
    if (native) {
        af_ok(
            core_prepare(fixture->engine,
                         "SELECT json FROM events WHERE operation_id=? AND type='tool.late_result'",
                         &stmt, &error),
            &error);
        sqlite3_bind_text(stmt, 1, operation.id, -1, SQLITE_TRANSIENT);
        assert(sqlite3_step(stmt) == SQLITE_ROW);
        const char *json = (const char *)sqlite3_column_text(stmt, 0);
        assert(strstr(json, "discarded") && !strstr(json, "base64"));
        sqlite3_finalize(stmt);
    }
    core_operation_free(&operation);
    af_close(fixture);
}

void agent_job_cases(void) {
    interrupt_case(false);
    interrupt_case(true);
}
