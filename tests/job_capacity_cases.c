#include "core_test.h"
#include "core/core.h"
#include "ah/jobs.h"
#include "agent_fixture.h"
#include <time.h>

typedef struct {
    ah_engine *engine;
    core_session *other;
    atomic_uint active, peak, entered;
    atomic_bool release;
    unsigned inline_calls;
} capacity_fixture;

static void pause_briefly(void) {
    struct timespec pause = {.tv_nsec = 1000000};
    nanosleep(&pause, NULL);
}

static ah_code counted(void *context, const ah_tool_call *call, ah_tool_result *result,
                       ah_error *error) {
    (void)error;
    capacity_fixture *f = context;
    unsigned active = atomic_fetch_add(&f->active, 1) + 1;
    unsigned peak = atomic_load(&f->peak);
    while (active > peak && !atomic_compare_exchange_weak(&f->peak, &peak, active)) {
    }
    if (!strcmp(call->arguments_json, "{\"inline\":true}")) {
        f->inline_calls++;
        if (call->yield)
            call->yield(call->yield_context);
    } else {
        atomic_fetch_add(&f->entered, 1);
        while (!atomic_load(&f->release) && !call->cancelled(call->cancel_context))
            pause_briefly();
    }
    atomic_fetch_sub(&f->active, 1);
    result->json = ah_strdup("{}");
    return AH_OK;
}

static void intent(ah_engine *e, core_session *session, const char *name, unsigned count) {
    char id[AH_ID_CAP];
    create_input(e, id);
    ah_error error = {0};
    ok(core_load(e, id, session, &error), &error);
    const char *names[] = {name, "parallel"}, *ids[] = {"first", "second"};
    const char *args[] = {strcmp(name, "serial") ? "{}" : "{\"inline\":true}", "{}"};
    char *response = af_calls(count, names, ids, args);
    ok(core_accept_response(e, session, response, &error), &error);
    free(response);
}

static void yield_at_capacity(void *context) {
    capacity_fixture *f = context;
    bool pending = false;
    ah_error error = {0};
    ok(core_tools_step(f->engine, f->other, &pending, &error), &error);
    assert(pending && !core_jobs_running(f->engine, NULL));
    assert(!atomic_load(&f->entered));
}

void job_capacity_cases(void) {
    for (unsigned mode = 0; mode < 3; mode++) {
        fixture provider = {0};
        ah_engine *e = open_engine(&provider, ":memory:", NULL);
        capacity_fixture f = {.engine = e};
        e->config.max_parallel_tools = mode == 1 ? 2 : 1;
        ah_tool tool = {.name = "background",
                        .description = "Capacity test",
                        .parameters_json = "{\"type\":\"object\"}",
                        .effect = AH_EFFECT_READ,
                        .execute = counted,
                        .context = &f};
        ah_error error = {0};
        ok(ah_register_background_tool(e, &tool, &error), &error);
        tool.name = "serial";
        ok(ah_register_tool(e, &tool, &error), &error);
        tool.name = "parallel";
        tool.parallel_safe = true;
        ok(ah_register_tool(e, &tool, &error), &error);
        core_session worker, owner;
        intent(e, &worker, "background", mode == 1 ? 2 : 1);
        intent(e, &owner, "serial", 1);
        bool pending = false;
        if (mode == 2) {
            f.other = &worker;
            e->yield = yield_at_capacity;
            e->yield_context = &f;
            ok(core_tools_step(e, &owner, &pending, &error), &error);
            assert(!pending && f.inline_calls == 1 && e->inline_tools == 0);
            e->yield = NULL;
            e->yield_context = NULL;
        }
        ok(core_tools_step(e, &worker, &pending, &error), &error);
        uint64_t deadline = ah_now_ms() + 5000;
        unsigned expected = mode == 1 ? 2 : 1;
        while (atomic_load(&f.entered) < expected && ah_now_ms() < deadline)
            pause_briefly();
        assert(atomic_load(&f.entered) == expected);
        if (mode != 2) {
            ok(core_tools_step(e, &owner, &pending, &error), &error);
            assert(pending && !f.inline_calls);
        }
        atomic_store(&f.release, true);
        while (core_jobs_running(e, NULL)) {
            core_jobs_wait(e, 20);
            ok(core_jobs_finish(e, &worker, &error), &error);
        }
        ok(core_tools_step(e, &owner, &pending, &error), &error);
        assert(!pending && f.inline_calls == 1 && atomic_load(&f.peak) == expected);
        core_session_free(&worker);
        core_session_free(&owner);
        ah_close(e);
    }
}
