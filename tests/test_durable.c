#include "durable_fixture.h"
#include "ah/jobs.h"
#include "ah/task.h"
#include "ah/work.h"
#include <time.h>

typedef struct {
    durable_fixture fixture;
    bool timer, timed_signal, early_signal, immediate;
    uint64_t until_ms;
    unsigned expected_signals;
} step_fixture;

static uint64_t wall_ms(void) {
    struct timespec value;
    assert(clock_gettime(CLOCK_REALTIME, &value) == 0);
    return (uint64_t)value.tv_sec * 1000 + (uint64_t)value.tv_nsec / 1000000;
}

static ah_code step(void *context, const ah_tool_call *call, ah_tool_result *result,
                    ah_error *error) {
    (void)error;
    step_fixture *state = context;
    durable_fixture *f = &state->fixture;
    assert(!pthread_equal(f->owner, pthread_self()));
    assert(call->checkpoint_json && call->generation > 0);
    assert(!call->checkpoint && !call->yield);
    unsigned active = atomic_fetch_add(&f->active, 1) + 1;
    unsigned peak = atomic_load(&f->peak);
    while (active > peak && !atomic_compare_exchange_weak(&f->peak, &peak, active)) {
    }
    atomic_fetch_add(&f->calls, 1);
    yyjson_doc *doc = yyjson_read(call->checkpoint_json, strlen(call->checkpoint_json), 0);
    assert(doc);
    unsigned phase = (unsigned)yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(doc), "phase"));
    yyjson_doc_free(doc);
    if (!phase) {
        assert(!call->wake_json);
        if (state->immediate)
            result->continuation_json = strdup("{\"checkpoint\":{\"phase\":1}}");
        else if (state->timer) {
            char continuation[256];
            snprintf(continuation, sizeof(continuation),
                     "{\"checkpoint\":{\"phase\":1},\"wait\":{%s\"until_ms\":%llu}}",
                     state->timed_signal ? "\"signal\":\"ready\"," : "",
                     (unsigned long long)state->until_ms);
            result->continuation_json = strdup(continuation);
        } else
            result->continuation_json =
                strdup("{\"checkpoint\":{\"phase\":1},\"wait\":{\"signal\":\"ready\"}}");
        assert(result->continuation_json);
    } else {
        assert(phase == 1);
        if (!state->immediate) {
            assert(call->wake_json);
            yyjson_doc *wake = yyjson_read(call->wake_json, strlen(call->wake_json), 0);
            assert(wake);
            yyjson_val *root = yyjson_doc_get_root(wake);
            const char *reason = yyjson_get_str(yyjson_obj_get(root, "reason"));
            assert(reason);
            if (state->timer && !state->early_signal)
                assert(!strcmp(reason, "timer") && wall_ms() >= state->until_ms);
            else {
                assert(!strcmp(reason, "signal"));
                yyjson_val *signal = yyjson_obj_get(root, "signal");
                assert(yyjson_equals_str(yyjson_obj_get(signal, "key"), "ready"));
                yyjson_val *value = yyjson_obj_get(signal, "value");
                assert(yyjson_get_uint(yyjson_obj_get(value, "value")) == 7);
                state->expected_signals++;
            }
            yyjson_doc_free(wake);
        }
        result->json = strdup("{\"value\":7}");
        assert(result->json);
    }
    atomic_fetch_sub(&f->active, 1);
    return AH_OK;
}

static void register_step(step_fixture *state, unsigned version, unsigned restarts) {
    ah_work_config work = {.version = version, .replay = AH_REPLAY_SAFE, .max_restarts = restarts};
    ah_tool tool = {.name = "durable",
                    .description = "Durable test phases",
                    .parameters_json = "{\"type\":\"object\",\"additionalProperties\":false}",
                    .effect = AH_EFFECT_READ,
                    .execute = step,
                    .context = state,
                    .work = &work};
    ah_error error = {0};
    df_ok(ah_register_background_tool(state->fixture.engine, &tool, &error), &error);
}

static void signal_work(step_fixture *state, const char *session, const char *operation,
                        const char *value, ah_signal_result expected) {
    ah_error error = {0};
    ah_signal_result actual;
    df_ok(ah_signal(state->fixture.engine, session, operation, "ready", value, &actual, &error),
          &error);
    assert(actual == expected);
}

static void signal_and_capacity(void) {
    step_fixture state = {.fixture = {.batch = 8, .signal_on_start = true}};
    durable_fixture *f = &state.fixture;
    df_open(f, ":memory:", 1);
    register_step(&state, 1, 0);
    char session[AH_ID_CAP];
    df_create(f, session);
    ah_error error = {0};
    df_ok(ah_run(f->engine, session, &error), &error);
    df_state(f, session, AH_WAITING_WORK);
    assert(f->models == 1 && f->started == f->batch);
    assert(atomic_load(&f->calls) == f->batch && atomic_load(&f->active) == 0);
    assert(atomic_load(&f->peak) == 1);
    assert(f->reentry_code == AH_BUSY);
    df_ok(ah_run(f->engine, session, &error), &error);
    assert(atomic_load(&f->calls) == f->batch);
    for (unsigned i = f->batch; i > 0; i--) {
        signal_work(&state, session, f->operations[i - 1], "{\"value\":7}", AH_SIGNAL_DELIVERED);
        signal_work(&state, session, f->operations[i - 1], "{\"value\":99}", AH_SIGNAL_DUPLICATE);
    }
    df_ok(ah_run(f->engine, session, &error), &error);
    df_state(f, session, AH_COMPLETED);
    assert(f->models == 2 && f->receipts == f->batch);
    assert(atomic_load(&f->calls) == 2 * f->batch && state.expected_signals == f->batch);
    for (unsigned i = 0; i < f->batch; i++)
        signal_work(&state, session, f->operations[i], "{\"value\":7}", AH_SIGNAL_CLOSED);
    assert(df_events(f, session, "tool.completed") == f->batch);
    ah_close(f->engine);
}

static void early_signal_and_idempotency(void) {
    char database[64], session[AH_ID_CAP], operation[AH_ID_CAP], same[AH_ID_CAP];
    df_database(database);
    step_fixture state = {0};
    durable_fixture *f = &state.fixture;
    df_open(f, database, 1);
    register_step(&state, 1, 0);
    ah_error error = {0};
    df_ok(ah_session_create(f->engine, "mock", "", session, &error), &error);
    df_ok(ah_work_start(f->engine, session, "root", "durable", "{}", operation, &error), &error);
    df_ok(ah_work_start(f->engine, session, "root", "durable", "{}", same, &error), &error);
    assert(!strcmp(operation, same));
    assert(ah_work_start(f->engine, session, "root", "missing", "{}", same, &error) == AH_INVALID);
    signal_work(&state, session, operation, "{\"value\":7}", AH_SIGNAL_DELIVERED);
    ah_signal_result delivery;
    assert(ah_signal(f->engine, session, operation, "invalid", "{", &delivery, &error) ==
           AH_INVALID);
    char other[AH_ID_CAP];
    df_ok(ah_session_create(f->engine, "mock", "", other, &error), &error);
    df_ok(ah_signal(f->engine, other, operation, "ready", "false", &delivery, &error), &error);
    assert(delivery == AH_SIGNAL_NOT_FOUND);
    df_ok(ah_submit(f->engine, other, "input", "Model session", &error), &error);
    assert(ah_work_start(f->engine, other, "root", "durable", "{}", same, &error) == AH_INVALID);
    ah_close(f->engine);
    df_open(f, database, 1);
    register_step(&state, 1, 0);
    signal_work(&state, session, operation, "{\"value\":99}", AH_SIGNAL_DUPLICATE);
    df_ok(ah_work_run(f->engine, session, &error), &error);
    assert(f->models == 0 && atomic_load(&f->calls) == 2 && state.expected_signals == 1);
    signal_work(&state, session, operation, "{\"value\":7}", AH_SIGNAL_CLOSED);
    signal_work(&state, session, "00000000-0000-0000-0000-000000000000", "{}", AH_SIGNAL_NOT_FOUND);
    df_ok(ah_work_run(f->engine, session, &error), &error);
    assert(atomic_load(&f->calls) == 2);
    char fork[AH_ID_CAP];
    assert(ah_fork(f->engine, session, fork, &error) == AH_INVALID);
    assert(ah_compact(f->engine, session, "summary", 0, &error) == AH_INVALID);
    ah_task_config task = {.contract_json = "{}",
                           .verifier_revision =
                               "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                           .max_model_calls = 1,
                           .max_tool_calls = 1,
                           .max_repairs = 1,
                           .timeout_ms = 1000};
    assert(ah_task_create(f->engine, session, &task, &error) == AH_INVALID);
    ah_close(f->engine);
    df_remove_database(database);
}

static void signal_capacity(bool timeout) {
    step_fixture state = {.timer = timeout, .timed_signal = timeout, .until_ms = wall_ms() + 300};
    durable_fixture *f = &state.fixture;
    df_open(f, ":memory:", 1);
    register_step(&state, 1, 0);
    char session[AH_ID_CAP], operation[AH_ID_CAP];
    ah_error error = {0};
    df_ok(ah_session_create(f->engine, "mock", "", session, &error), &error);
    df_ok(ah_work_start(f->engine, session, "root", "durable", "{}", operation, &error), &error);
    df_ok(ah_work_run(f->engine, session, &error), &error);
    assert(atomic_load(&f->calls) == 1);
    ah_signal_result delivery;
    for (unsigned i = 0; i < 127; i++) {
        char key[32];
        snprintf(key, sizeof(key), "unrelated_%u", i);
        df_ok(ah_signal(f->engine, session, operation, key, "true", &delivery, &error), &error);
        assert(delivery == AH_SIGNAL_DELIVERED);
    }
    assert(ah_signal(f->engine, session, operation, "overflow", "true", &delivery, &error) ==
           AH_LIMIT);
    if (timeout) {
        while (wall_ms() < state.until_ms)
            df_sleep(5);
        signal_work(&state, session, operation, "{\"value\":7}", AH_SIGNAL_CLOSED);
    } else
        signal_work(&state, session, operation, "{\"value\":7}", AH_SIGNAL_DELIVERED);
    df_ok(ah_work_run(f->engine, session, &error), &error);
    assert(atomic_load(&f->calls) == 2);
    ah_close(f->engine);
}

static void timer_reopen(unsigned signal_mode) {
    char database[64], session[AH_ID_CAP], operation[AH_ID_CAP];
    df_database(database);
    step_fixture state = {.timer = true,
                          .timed_signal = signal_mode != 0,
                          .early_signal = signal_mode == 2,
                          .until_ms = wall_ms() + 150};
    durable_fixture *f = &state.fixture;
    df_open(f, database, 1);
    register_step(&state, 1, 0);
    ah_error error = {0};
    df_ok(ah_session_create(f->engine, "mock", "", session, &error), &error);
    df_ok(ah_work_start(f->engine, session, "timer", "durable", "{}", operation, &error), &error);
    df_ok(ah_work_run(f->engine, session, &error), &error);
    assert(atomic_load(&f->calls) == 1);
    uint64_t wake = 0;
    df_ok(ah_work_next_wake(f->engine, session, &wake, &error), &error);
    assert(wake == state.until_ms);
    df_ok(ah_work_run(f->engine, session, &error), &error);
    assert(atomic_load(&f->calls) == 1);
    if (signal_mode == 2)
        signal_work(&state, session, operation, "{\"value\":7}", AH_SIGNAL_DELIVERED);
    ah_close(f->engine);
    while (wall_ms() < state.until_ms)
        df_sleep(5);
    df_open(f, database, 1);
    register_step(&state, 1, 0);
    if (signal_mode == 1)
        signal_work(&state, session, operation, "{\"value\":7}", AH_SIGNAL_CLOSED);
    df_ok(ah_work_run(f->engine, session, &error), &error);
    assert(f->models == 0 && atomic_load(&f->calls) == 2);
    df_ok(ah_work_next_wake(f->engine, session, &wake, &error), &error);
    assert(wake == 0);
    df_ok(ah_work_run(f->engine, session, &error), &error);
    assert(atomic_load(&f->calls) == 2);
    ah_close(f->engine);
    df_remove_database(database);
}

static void immediate_continue(void) {
    step_fixture state = {.immediate = true};
    durable_fixture *f = &state.fixture;
    df_open(f, ":memory:", 1);
    register_step(&state, 1, 0);
    char session[AH_ID_CAP];
    df_create(f, session);
    ah_error error = {0};
    df_ok(ah_run(f->engine, session, &error), &error);
    df_state(f, session, AH_COMPLETED);
    assert(atomic_load(&f->calls) == 2 && f->models == 2 && f->receipts == 1);
    ah_close(f->engine);
}

static ah_code second_wait(void *context, const ah_tool_call *call, ah_tool_result *result,
                           ah_error *error) {
    (void)error;
    step_fixture *state = context;
    unsigned invocation = atomic_fetch_add(&state->fixture.calls, 1);
    if (invocation == 0) {
        char continuation[256];
        snprintf(continuation, sizeof(continuation),
                 "{\"checkpoint\":{\"phase\":1},\"wait\":{\"signal\":\"first\","
                 "\"until_ms\":%llu}}",
                 (unsigned long long)state->until_ms);
        result->continuation_json = strdup(continuation);
    } else if (invocation == 1) {
        yyjson_doc *wake = yyjson_read(call->wake_json, strlen(call->wake_json), 0);
        assert(wake &&
               yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(wake), "reason"), "timer"));
        yyjson_doc_free(wake);
        result->continuation_json =
            strdup("{\"checkpoint\":{\"phase\":2},\"wait\":{\"signal\":\"second\"}}");
    } else {
        assert(invocation == 2);
        yyjson_doc *wake = yyjson_read(call->wake_json, strlen(call->wake_json), 0);
        assert(wake);
        yyjson_val *signal = yyjson_obj_get(yyjson_doc_get_root(wake), "signal");
        assert(yyjson_equals_str(yyjson_obj_get(signal, "key"), "second"));
        yyjson_doc_free(wake);
        result->json = strdup("{}");
    }
    assert(result->json || result->continuation_json);
    return AH_OK;
}

static void expired_signal_stays_closed(void) {
    step_fixture state = {.until_ms = wall_ms() + 100};
    durable_fixture *f = &state.fixture;
    df_open(f, ":memory:", 1);
    ah_work_config work = {.version = 1, .replay = AH_REPLAY_SAFE};
    ah_tool tool = {.name = "durable",
                    .description = "Two independent signal waits",
                    .parameters_json = "{\"type\":\"object\"}",
                    .effect = AH_EFFECT_READ,
                    .execute = second_wait,
                    .context = &state,
                    .work = &work};
    ah_error error = {0};
    df_ok(ah_register_tool(f->engine, &tool, &error), &error);
    char session[AH_ID_CAP], operation[AH_ID_CAP];
    df_ok(ah_session_create(f->engine, "mock", "", session, &error), &error);
    df_ok(ah_work_start(f->engine, session, "root", "durable", "{}", operation, &error), &error);
    df_ok(ah_work_run(f->engine, session, &error), &error);
    while (wall_ms() < state.until_ms)
        df_sleep(5);
    df_ok(ah_work_run(f->engine, session, &error), &error);
    assert(atomic_load(&f->calls) == 2);
    ah_signal_result delivery;
    df_ok(ah_signal(f->engine, session, operation, "first", "true", &delivery, &error), &error);
    assert(delivery == AH_SIGNAL_CLOSED);
    df_ok(ah_signal(f->engine, session, operation, "second", "true", &delivery, &error), &error);
    assert(delivery == AH_SIGNAL_DELIVERED);
    df_ok(ah_work_run(f->engine, session, &error), &error);
    assert(atomic_load(&f->calls) == 3);
    ah_close(f->engine);
}

static void cancel_parked(void) {
    step_fixture state = {0};
    durable_fixture *f = &state.fixture;
    df_open(f, ":memory:", 1);
    register_step(&state, 1, 0);
    char session[AH_ID_CAP], operation[AH_ID_CAP];
    ah_error error = {0};
    df_ok(ah_session_create(f->engine, "mock", "", session, &error), &error);
    df_ok(ah_work_start(f->engine, session, "root", "durable", "{}", operation, &error), &error);
    df_ok(ah_work_run(f->engine, session, &error), &error);
    assert(atomic_load(&f->calls) == 1);
    df_ok(ah_work_cancel(f->engine, session, operation, &error), &error);
    signal_work(&state, session, operation, "{\"value\":7}", AH_SIGNAL_CLOSED);
    df_ok(ah_work_run(f->engine, session, &error), &error);
    assert(atomic_load(&f->calls) == 1 && !state.expected_signals);
    ah_close(f->engine);
}

typedef struct {
    durable_fixture fixture;
    atomic_bool entered;
    char operation[AH_ID_CAP];
} cancel_fixture;

static ah_code late_continuation(void *context, const ah_tool_call *call, ah_tool_result *result,
                                 ah_error *error) {
    (void)error;
    cancel_fixture *f = context;
    atomic_fetch_add(&f->fixture.calls, 1);
    atomic_store(&f->entered, true);
    uint64_t deadline = ah_now_ms() + 2000;
    while (!call->cancelled(call->cancel_context) && ah_now_ms() < deadline)
        df_sleep(1);
    assert(call->cancelled(call->cancel_context));
    result->continuation_json =
        strdup("{\"checkpoint\":{\"late\":true},\"wait\":{\"signal\":\"ready\"}}");
    assert(result->continuation_json);
    return AH_OK;
}

static void *cancel_active(void *context) {
    cancel_fixture *f = context;
    uint64_t deadline = ah_now_ms() + 2000;
    while (!atomic_load(&f->entered) && ah_now_ms() < deadline)
        df_sleep(1);
    assert(atomic_load(&f->entered));
    ah_error error = {0};
    df_ok(ah_cancel_job(f->fixture.engine, f->operation, &error), &error);
    return NULL;
}

static void cancelled_continuation_cannot_revive(void) {
    cancel_fixture state = {0};
    durable_fixture *f = &state.fixture;
    df_open(f, ":memory:", 1);
    ah_work_config work = {.version = 1, .replay = AH_REPLAY_SAFE};
    ah_tool tool = {.name = "durable",
                    .description = "Late continuation after cancellation",
                    .parameters_json = "{\"type\":\"object\"}",
                    .effect = AH_EFFECT_READ,
                    .execute = late_continuation,
                    .context = &state,
                    .work = &work};
    ah_error error = {0};
    df_ok(ah_register_background_tool(f->engine, &tool, &error), &error);
    char session[AH_ID_CAP];
    df_ok(ah_session_create(f->engine, "mock", "", session, &error), &error);
    df_ok(ah_work_start(f->engine, session, "root", "durable", "{}", state.operation, &error),
          &error);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, cancel_active, &state) == 0);
    assert(ah_work_run(f->engine, session, &error) == AH_CANCELLED);
    assert(pthread_join(thread, NULL) == 0);
    ah_signal_result delivery;
    df_ok(ah_signal(f->engine, session, state.operation, "ready", "true", &delivery, &error),
          &error);
    assert(delivery == AH_SIGNAL_CLOSED);
    df_ok(ah_resume(f->engine, session, &error), &error);
    df_ok(ah_work_run(f->engine, session, &error), &error);
    assert(atomic_load(&f->calls) == 1);
    assert(df_events(f, session, "work.parked") == 0);
    ah_close(f->engine);
}

void df_deferred_cases(void);
void durable_receipt_cases(void);

int main(int argc, char **argv) {
    int child = df_recovery_child(argc, argv);
    if (child >= 0)
        return child;
    signal_and_capacity();
    early_signal_and_idempotency();
    signal_capacity(false);
    signal_capacity(true);
    timer_reopen(0);
    timer_reopen(1);
    timer_reopen(2);
    immediate_continue();
    expired_signal_stays_closed();
    df_composition_cases();
    cancel_parked();
    cancelled_continuation_cannot_revive();
    df_recovery_cases(argv[0]);
    df_deferred_cases();
    durable_receipt_cases();
    puts("durable steps, signals, capacity, timers and crash recovery passed");
    return 0;
}
