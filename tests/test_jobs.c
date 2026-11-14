#include "core_test.h"
#include "agent_fixture.h"
#include "core/core.h"
#include "ah/jobs.h"
#include <fcntl.h>
#include <stdatomic.h>
#include <time.h>

static const char object_schema[] = "{\"type\":\"object\",\"additionalProperties\":false}";

static ah_local_tools *local(ah_engine *engine, const char *workspace) {
    ah_local_tools_config config = {.workspace = workspace, .enable_exec = true};
    ah_local_tools *tools = NULL;
    ah_error error = {0};
    ok(ah_local_tools_open(&config, &tools, &error), &error);
    ok(ah_local_tools_register(tools, engine, &error), &error);
    assert(core_find_tool(engine, "exec")->background);
    return tools;
}

static char *exec_response(const char *script) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc), *argv = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_val(doc, root, "argv", argv);
    yyjson_mut_arr_add_str(doc, argv, "/bin/sh");
    yyjson_mut_arr_add_str(doc, argv, "-c");
    yyjson_mut_arr_add_str(doc, argv, script);
    char *args = yyjson_mut_write(doc, 0, NULL);
    char *reply = af_call("exec", "process", args);
    free(args);
    yyjson_mut_doc_free(doc);
    return reply;
}

static void approval_and_timeout(void) {
    char workspace[] = "/tmp/ah-job-exec-XXXXXX";
    assert(mkdtemp(workspace));
    char marker[256];
    snprintf(marker, sizeof(marker), "%s/marker", workspace);
    char *response = exec_response("printf x >> marker; sleep 2; printf completed");
    fixture f = {.answers = {response, done}};
    ah_config config;
    ah_config_init(&config);
    config.tool_timeout_ms = 80;
    ah_engine *engine = open_engine(&f, ":memory:", &config);
    ah_local_tools *tools = local(engine, workspace);
    char session[AH_ID_CAP];
    create_input(engine, session);
    ah_error error = {0};
    ok(ah_run(engine, session, &error), &error);
    state_is(engine, session, "waiting_approval");
    assert(access(marker, F_OK) < 0);
    char *operation = pending(engine, session);
    ok(ah_decide(engine, session, operation, true, &error), &error);
    assert(ah_run(engine, session, &error) == AH_UNCERTAIN);
    assert(!core_jobs_running(engine, NULL));
    state_is(engine, session, "outcome_unknown");
    assert(ah_resume(engine, session, &error) == AH_UNCERTAIN);
    ok(ah_resolve(engine, session, operation, "{\"reconciled\":true}", false, &error), &error);
    ok(ah_run(engine, session, &error), &error);
    state_is(engine, session, "completed");
    ok(ah_run(engine, session, &error), &error);
    assert(f.calls == 2);
    FILE *file = fopen(marker, "rb");
    assert(file && fgetc(file) == 'x' && fgetc(file) == EOF);
    fclose(file);
    assert(af_occurrences(f.last_request, "function_call_output") == 1);
    ah_close(engine);
    ah_local_tools_close(tools);
    free(f.last_request);
    free(operation);
    free(response);
    assert(unlink(marker) == 0 && rmdir(workspace) == 0);
}

typedef struct {
    ah_engine *engine;
    pthread_t owner;
    atomic_bool started;
    char operation[AH_ID_CAP];
    unsigned calls;
} cancellation;

static ah_code cancellable(void *context, const ah_tool_call *call, ah_tool_result *result,
                           ah_error *error) {
    (void)error;
    cancellation *state = context;
    assert(!pthread_equal(state->owner, pthread_self()));
    assert(!call->checkpoint && !call->yield);
    state->calls++;
    memcpy(state->operation, call->operation_id, AH_ID_CAP);
    atomic_store(&state->started, true);
    struct timespec pause = {.tv_nsec = 1000000};
    while (!call->cancelled(call->cancel_context))
        nanosleep(&pause, NULL);
    result->json = strdup("{\"cancelled\":true}");
    return AH_OK;
}

static void *cancel_one(void *context) {
    cancellation *state = context;
    struct timespec pause = {.tv_nsec = 1000000};
    while (!atomic_load(&state->started))
        nanosleep(&pause, NULL);
    ah_error error = {0};
    ok(ah_cancel_job(state->engine, state->operation, &error), &error);
    return NULL;
}

static void cancellation_receipt(void) {
    char *response = af_call("background", "cancel", "{}");
    fixture f = {.answers = {response, done}};
    ah_engine *engine = open_engine(&f, ":memory:", NULL);
    cancellation state = {.engine = engine, .owner = pthread_self()};
    atomic_init(&state.started, false);
    ah_tool tool = {.name = "background",
                    .description = "Cooperative cancellation",
                    .parameters_json = object_schema,
                    .effect = AH_EFFECT_READ,
                    .execute = cancellable,
                    .context = &state};
    ah_error error = {0};
    ok(ah_register_background_tool(engine, &tool, &error), &error);
    char session[AH_ID_CAP];
    create_input(engine, session);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, cancel_one, &state) == 0);
    assert(ah_run(engine, session, &error) == AH_CANCELLED);
    pthread_join(thread, NULL);
    assert(!core_jobs_running(engine, NULL) && state.calls == 1);
    assert(ah_cancel_job(engine, state.operation, &error) == AH_NOT_FOUND);
    state_is(engine, session, "stopped");
    ok(ah_resume(engine, session, &error), &error);
    ok(ah_run(engine, session, &error), &error);
    assert(state.calls == 1 && af_occurrences(f.last_request, "function_call_output") == 1);
    ah_close(engine);
    free(response);
    free(f.last_request);
}

static char *overlap_reply(agent_fixture *f, agent_request *request) {
    if (!strcmp(request->session, f->root)) {
        if (request->ordinal == 1) {
            const char *names[] = {"spawn", "spawn"}, *ids[] = {"one", "two"};
            const char *args[] = {"{\"name\":\"one\",\"message\":\"ONE\"}",
                                  "{\"name\":\"two\",\"message\":\"TWO\"}"};
            return af_calls(2, names, ids, args);
        }
        af_spawn_id(request->request, "one", f->ids[0]);
        af_spawn_id(request->request, "two", f->ids[1]);
        return af_text("ROOT_DONE");
    }
    if (request->ordinal == 1)
        return exec_response("sleep 0.25; printf fixed_output");
    yyjson_doc *receipt = af_receipt(request->request, "process");
    assert(yyjson_equals_str(yyjson_obj_get(af_result(receipt), "stdout"), "fixed_output"));
    yyjson_doc_free(receipt);
    return af_text("CHILD_DONE");
}

static void overlap_events(agent_fixture *f, const ah_event *event) {
    if (!strcmp(event->type, "tool.started") && strstr(event->json, "fixed_output")) {
        f->phase++;
        if (f->phase > f->barrier)
            f->barrier = f->phase;
    }
    if (!strcmp(event->type, "tool.completed") && strstr(event->json, "fixed_output")) {
        assert(f->phase);
        f->phase--;
    }
}

static uint64_t overlap(unsigned workers) {
    agent_fixture f = {.reply = overlap_reply};
    af_open(&f, 3, 8, 4);
    f.engine->config.max_parallel_tools = workers;
    ah_local_tools *tools = local(f.engine, "/tmp");
    af_create(&f, "OVERLAP");
    af_run(&f);
    ah_error error = {0};
    for (unsigned i = 0; i < 2; i++) {
        af_state(&f, f.ids[i], "waiting_approval");
        char *operation = pending(f.engine, f.ids[i]);
        ok(ah_decide(f.engine, f.ids[i], operation, true, &error), &error);
        free(operation);
    }
    f.on_event = overlap_events;
    uint64_t began = ah_now_ms();
    af_run(&f);
    uint64_t elapsed = ah_now_ms() - began;
    assert(f.barrier == workers && f.phase == 0 && !core_jobs_running(f.engine, NULL));
    for (unsigned i = 0; i < 2; i++)
        af_state(&f, f.ids[i], "completed");
    af_close(&f);
    ah_local_tools_close(tools);
    return elapsed;
}

int job_recovery_child(int, char **);
void job_recovery_cases(const char *);
void agent_job_cases(void);

int main(int argc, char **argv) {
    int child = job_recovery_child(argc, argv);
    if (child >= 0)
        return child;
    job_recovery_cases(argv[0]);
    approval_and_timeout();
    cancellation_receipt();
    agent_job_cases();
    uint64_t sequential = overlap(1), concurrent = overlap(2);
    assert(concurrent * 4 < sequential * 3);
    printf("native exec fixed receipts: sequential=%llu ms concurrent=%llu ms\n",
           (unsigned long long)sequential, (unsigned long long)concurrent);
    return 0;
}
