#include "parallel_test.h"
#include "ah/jobs.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static const char schema[] = "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"}},"
                             "\"required\":[\"id\"],\"additionalProperties\":false}";
static const char done[] =
    "{\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
    "\"status\":\"completed\",\"content\":[{\"type\":\"output_text\",\"text\":\"done\"}]}]}";

void ok(ah_code code, const ah_error *error) {
    if (code != AH_OK) {
        fprintf(stderr, "%s: %s\n", ah_code_name(code), error->message);
        abort();
    }
}

static unsigned argument_id(const char *json) {
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    assert(doc);
    unsigned id = (unsigned)yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(doc), "id"));
    yyjson_doc_free(doc);
    assert(id && id <= CALLS);
    return id;
}

static void observe(void *context, const ah_event *event) {
    fixture *f = context;
    assert(pthread_equal(pthread_self(), f->owner));
    if (f->verify_timing && !strcmp(event->type, "runtime.metrics")) {
        yyjson_doc *doc = yyjson_read(event->json, strlen(event->json), 0);
        yyjson_val *root = yyjson_doc_get_root(doc);
        if (yyjson_equals_str(yyjson_obj_get(root, "phase"), "tool")) {
            unsigned id = 1;
            while (id <= f->count && strcmp(f->operations[id], event->operation_id))
                id++;
            f->timed++;
            uint64_t start = yyjson_get_uint(yyjson_obj_get(root, "started_us"));
            uint64_t duration = yyjson_get_uint(yyjson_obj_get(root, "duration_us"));
            assert(id <= f->count && start <= f->began[id] && start + duration >= f->ended[id]);
        }
        yyjson_doc_free(doc);
    }
    if (!strcmp(event->type, "tool.started")) {
        unsigned id = argument_id(event->json);
        memcpy(f->operations[id], event->operation_id, AH_ID_CAP);
        f->started++;
        if (f->started == 1 && f->expected_batch) {
            assert(sqlite3_get_autocommit(f->engine->db));
            assert(scalar(f, "SELECT COUNT(*) FROM operations WHERE status='executing'") ==
                   (int)f->expected_batch);
            assert(scalar(f, "SELECT tool_calls FROM sessions") == (int)f->expected_batch);
        }
        if (f->cancel_before_launch)
            ah_cancel(f->engine);
    }
    if (!strcmp(event->type, "tool.received") || !strcmp(event->type, "tool.received_late")) {
        if (f->crash_on_capture)
            _exit(77);
        unsigned id = 1;
        while (id <= f->count && strcmp(f->operations[id], event->operation_id))
            id++;
        assert(id <= f->count);
        if (f->verify_capture && id == 2) {
            assert(!f->receipts);
            assert(scalar(f, "SELECT COUNT(*) FROM operations WHERE status='received'") == 1);
        }
        pthread_mutex_lock(&f->mutex);
        f->captured[id] = true;
        pthread_cond_broadcast(&f->changed);
        pthread_mutex_unlock(&f->mutex);
    }
    if (!strcmp(event->type, "tool.completed") || !strcmp(event->type, "tool.late_result")) {
        f->receipts++;
        if (!strcmp(event->type, "tool.late_result"))
            f->late_receipts++;
    }
    if (!f->interrupted && ((f->interrupt_intent && !strcmp(event->type, "tool.started")) ||
                            (f->interrupt_event && !strcmp(event->type, f->interrupt_event)))) {
        f->interrupted = true;
        sqlite3_interrupt(f->engine->db);
    }
}

static ah_code complete(void *context, const char *request, ah_delta_sink delta,
                        void *delta_context, ah_cancel_check cancelled, void *cancel_context,
                        char **response, ah_error *error) {
    (void)delta;
    (void)delta_context;
    (void)error;
    fixture *f = context;
    assert(pthread_equal(pthread_self(), f->owner));
    assert(!cancelled(cancel_context));
    free(f->last_request);
    f->last_request = ah_strdup(request);
    assert(f->last_request);
    if (f->provider_calls++) {
        *response = ah_strdup(done);
        return AH_OK;
    }
    char json[4096];
    size_t used = (size_t)snprintf(json, sizeof(json), "{\"status\":\"completed\",\"output\":[");
    for (unsigned i = 0; i < f->count; i++) {
        char arguments[80] = "{}";
        if (!strcmp(f->names[i], "harness_ask"))
            snprintf(arguments, sizeof(arguments), "{\\\"question\\\":\\\"choose\\\"}");
        else if (!strcmp(f->names[i], "harness_wait"))
            snprintf(arguments, sizeof(arguments), "{\\\"reason\\\":\\\"wait\\\"}");
        else if (f->invalid_id != i + 1)
            snprintf(arguments, sizeof(arguments), "{\\\"id\\\":%u}", i + 1);
        int written = snprintf(json + used, sizeof(json) - used,
                               "%s{\"type\":\"function_call\",\"call_id\":\"call_%u\","
                               "\"name\":\"%s\",\"arguments\":\"%s\"}",
                               i ? "," : "", i + 1, f->names[i], arguments);
        assert(written > 0 && (size_t)written < sizeof(json) - used);
        used += (size_t)written;
    }
    assert(snprintf(json + used, sizeof(json) - used, "]}") == 2);
    *response = ah_strdup(json);
    assert(*response);
    return AH_OK;
}

static void wait_until(fixture *f, bool *condition) {
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 5;
    while (!*condition) {
        int rc = pthread_cond_timedwait(&f->changed, &f->mutex, &deadline);
        assert(rc == 0 || rc == ETIMEDOUT);
        if (rc == ETIMEDOUT) {
            f->timed_out = true;
            break;
        }
    }
}

static ah_code execute(void *context, const ah_tool_call *call, ah_tool_result *result,
                       ah_error *error) {
    fixture *f = context;
    unsigned id = argument_id(call->arguments_json);
    f->began[id] = ah_now_us();
    assert(call->deadline_ms);
    assert(f->started >= f->expected_starts[id]);
    assert(f->receipts >= f->minimum_receipts[id]);
    if (f->expect_owner)
        assert(pthread_equal(pthread_self(), f->owner));
    if (f->wait_yield)
        assert(!call->yield && !call->yield_context);
    pthread_mutex_lock(&f->mutex);
    assert(!f->entered[id]);
    f->entered[id] = true;
    f->executions++;
    f->active++;
    if (f->active > f->max_active)
        f->max_active = f->active;
    pthread_cond_broadcast(&f->changed);
    if (!strcmp(f->names[id - 1], "write")) {
        assert(f->active == 1);
        for (unsigned previous = 1; previous < id; previous++)
            assert(f->finished[previous]);
        for (unsigned next = id + 1; next <= f->count; next++)
            assert(!f->entered[next]);
    }
    for (unsigned other = 1; other <= f->barrier_count; other++)
        wait_until(f, &f->entered[other]);
    if (f->partner[id])
        wait_until(f, &f->entered[f->partner[id]]);
    if (f->wait_finished[id])
        wait_until(f, &f->finished[f->wait_finished[id]]);
    if (f->wait_captured[id])
        wait_until(f, &f->captured[f->wait_captured[id]]);
    if (f->wait_yield)
        wait_until(f, &f->yielded);
    if (f->cancel_id == id)
        ah_cancel(f->engine);
    if (f->wait_deadline) {
        pthread_mutex_unlock(&f->mutex);
        while (!call->cancelled(call->cancel_context)) {
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
        pthread_mutex_lock(&f->mutex);
    }
    f->active--;
    f->finished[id] = true;
    f->finish_order[f->finished_count++] = id;
    pthread_cond_broadcast(&f->changed);
    pthread_mutex_unlock(&f->mutex);
    f->ended[id] = ah_now_us();
    if (f->error_id == id)
        return ah_fail(error, AH_IO, "injected read error");
    char json[40];
    snprintf(json, sizeof(json), "{\"id\":%u}", id);
    result->json = ah_strdup(json);
    result->outcome_unknown = f->unknown[id];
    assert(result->json);
    return AH_OK;
}

static void register_tools(fixture *f) {
    ah_error error = {0};
    ah_tool tools[] = {
        {"read", "Concurrent read", schema, AH_EFFECT_READ, false, execute, f, true, NULL},
        {"serial", "Unmarked read", schema, AH_EFFECT_READ, false, execute, f, false, NULL},
        {"write", "Write barrier", schema, AH_EFFECT_WRITE, false, execute, f, false, NULL},
        {"approved", "Approval barrier", schema, AH_EFFECT_READ, true, execute, f, true, NULL},
    };
    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++)
        ok(ah_register_tool(f->engine, &tools[i], &error), &error);
    ah_tool background = {
        "background", "Background read", schema, AH_EFFECT_READ, false, execute, f, false, NULL};
    ok(ah_register_background_tool(f->engine, &background, &error), &error);
}

void open_fixture(fixture *f, unsigned parallel, unsigned budget, uint64_t timeout) {
    f->owner = pthread_self();
    assert(pthread_mutex_init(&f->mutex, NULL) == 0);
    assert(pthread_cond_init(&f->changed, NULL) == 0);
    ah_config config;
    ah_config_init(&config);
    config.database_path = f->database_path ? f->database_path : ":memory:";
    config.provider = (ah_provider){complete, f};
    config.observer = observe;
    config.observer_context = f;
    config.max_parallel_tools = parallel;
    config.max_tool_calls = budget;
    config.tool_timeout_ms = timeout;
    ah_error error = {0};
    ok(ah_open(&config, &f->engine, &error), &error);
    register_tools(f);
}

void reopen_fixture(fixture *f) {
    ah_config config = f->engine->config;
    ah_close(f->engine);
    f->engine = NULL;
    ah_error error = {0};
    ok(ah_open(&config, &f->engine, &error), &error);
    register_tools(f);
}

void close_fixture(fixture *f) {
    assert(!f->active && !f->timed_out);
    ah_close(f->engine);
    free(f->last_request);
    assert(pthread_cond_destroy(&f->changed) == 0);
    assert(pthread_mutex_destroy(&f->mutex) == 0);
}

ah_code run_fixture(fixture *f, char sid[AH_ID_CAP]) {
    ah_error error = {0};
    ok(ah_session_create(f->engine, "mock", "test", sid, &error), &error);
    ok(ah_submit(f->engine, sid, "command", "run tools", &error), &error);
    return ah_run(f->engine, sid, &error);
}

int scalar(fixture *f, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(f->engine->db, sql, -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    int value = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return value;
}

void assert_receipt_order(fixture *f, unsigned count) {
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(f->engine->db,
                              "SELECT json FROM items WHERE json LIKE '%function_call_output%' "
                              "ORDER BY seq",
                              -1, &stmt, NULL) == SQLITE_OK);
    for (unsigned i = 1; i <= count; i++) {
        assert(sqlite3_step(stmt) == SQLITE_ROW);
        const char *json = (const char *)sqlite3_column_text(stmt, 0);
        yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
        assert(doc);
        char expected[32];
        snprintf(expected, sizeof(expected), "call_%u", i);
        assert(
            !strcmp(yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "call_id")), expected));
        yyjson_doc_free(doc);
    }
    assert(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);

    assert(sqlite3_prepare_v2(f->engine->db,
                              "SELECT operations.call_id FROM events JOIN operations "
                              "ON operations.id=events.operation_id "
                              "WHERE type IN ('tool.completed','tool.late_result',"
                              "'tool.interrupted') "
                              "ORDER BY events.seq",
                              -1, &stmt, NULL) == SQLITE_OK);
    for (unsigned i = 1; i <= count; i++) {
        assert(sqlite3_step(stmt) == SQLITE_ROW);
        char expected[32];
        snprintf(expected, sizeof(expected), "call_%u", i);
        assert(!strcmp((const char *)sqlite3_column_text(stmt, 0), expected));
    }
    assert(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);

    if (f->provider_calls > 1) {
        yyjson_doc *doc = yyjson_read(f->last_request, strlen(f->last_request), 0);
        assert(doc);
        yyjson_val *input = yyjson_obj_get(yyjson_doc_get_root(doc), "input"), *item;
        size_t index, size;
        unsigned seen = 0;
        yyjson_arr_foreach(input, index, size, item) {
            const char *type = yyjson_get_str(yyjson_obj_get(item, "type"));
            if (!type || strcmp(type, "function_call_output"))
                continue;
            char expected[32];
            snprintf(expected, sizeof(expected), "call_%u", ++seen);
            assert(!strcmp(yyjson_get_str(yyjson_obj_get(item, "call_id")), expected));
        }
        assert(seen == count);
        yyjson_doc_free(doc);
    }
}
