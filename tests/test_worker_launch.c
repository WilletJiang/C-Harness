#include "core_test.h"
#include "core/core.h"
#include "agent_fixture.h"
#include "ah/jobs.h"
#include <dlfcn.h>
#include <errno.h>

static int fail_after = -1;

/* Failure injection belongs to this executable, never the production executor. */
int pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*start)(void *),
                   void *argument) {
    if (fail_after == 0) {
        fail_after = -1;
        return EAGAIN;
    }
    if (fail_after > 0)
        fail_after--;
    void *symbol = dlsym(RTLD_NEXT, "pthread_create");
    int (*create)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
    _Static_assert(sizeof(create) == sizeof(symbol), "function pointer size");
    memcpy(&create, &symbol, sizeof(create));
    assert(create);
    return create(thread, attributes, start, argument);
}

static ah_code callback(void *context, const ah_tool_call *call, ah_tool_result *result,
                        ah_error *error) {
    (void)call;
    (void)error;
    atomic_uint *calls = context;
    atomic_fetch_add(calls, 1);
    result->json = ah_strdup("{}");
    return AH_OK;
}

int main(void) {
    for (unsigned mode = 0; mode < 3; mode++) {
        const char *names[] = {"worker", "worker"}, *ids[] = {"one", "two"};
        const char *args[] = {"{}", "{}"};
        unsigned count = mode == 2 ? 1 : 2;
        char *response = af_calls(count, names, ids, args);
        fixture f = {.answers = {response, done}};
        ah_engine *e = open_engine(&f, ":memory:", NULL);
        atomic_uint calls = 0;
        ah_tool tool = {.name = "worker",
                        .description = "Launch failure test",
                        .parameters_json = "{\"type\":\"object\"}",
                        .effect = mode == 2 ? AH_EFFECT_EXTERNAL : AH_EFFECT_READ,
                        .execute = callback,
                        .context = &calls,
                        .parallel_safe = true};
        ah_error error = {0};
        if (mode == 2)
            ok(ah_register_background_tool(e, &tool, &error), &error);
        else
            ok(ah_register_tool(e, &tool, &error), &error);
        char sid[AH_ID_CAP];
        create_input(e, sid);
        fail_after = mode == 1 ? 1 : 0;
        assert(ah_run(e, sid, &error) == AH_IO);
        assert(fail_after == -1 && !core_jobs_running(e, NULL));
        assert(atomic_load(&calls) <= (mode == 1 ? 1u : 0u));
        state_is(e, sid, "failed");
        sqlite3_stmt *stmt = NULL;
        ok(core_prepare(e, "SELECT COUNT(*) FROM operations WHERE status='done'", &stmt, &error),
           &error);
        assert(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == (int)count);
        sqlite3_finalize(stmt);
        unsigned before = atomic_load(&calls);
        ok(ah_resume(e, sid, &error), &error);
        ok(ah_run(e, sid, &error), &error);
        assert(atomic_load(&calls) == before && f.calls == 2);
        assert(af_occurrences(f.last_request, "function_call_output") == count);
        ah_close(e);
        free(response);
        free(f.last_request);
    }
    puts("worker launch failure tests passed");
    return 0;
}
