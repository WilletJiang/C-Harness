/* Deterministic local callbacks; no network or model inference.
 * AH_BENCH_ORDINARY_ONLY compares this workload against pre-work headers. */
#include "ah/ah.h"
#ifndef AH_BENCH_ORDINARY_ONLY
#include "ah/work.h"
#endif
#include "ah/jobs.h"
#include "yyjson.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#elif defined(__linux__)
#include <dirent.h>
#endif

#define SAMPLES 7
#define WARMUPS 1
#define OPERATIONS 64
#define PARKED 256

typedef struct {
    atomic_uint callbacks;
    unsigned models, receipts;
    bool park;
} fixture;

static void check(bool valid, const char *message) {
    if (!valid) {
        fprintf(stderr, "durable benchmark: %s\n", message);
        exit(1);
    }
}

static void ok(ah_code code, const ah_error *error) {
    if (code != AH_OK) {
        fprintf(stderr, "%s: %s\n", ah_code_name(code), error->message);
        exit(1);
    }
}

static double now_ms(void) {
    struct timespec value;
    check(clock_gettime(CLOCK_MONOTONIC, &value) == 0, "cannot read clock");
    return (double)value.tv_sec * 1000 + (double)value.tv_nsec / 1000000;
}

#ifndef AH_BENCH_ORDINARY_ONLY
static double cpu_ms(void) {
    struct rusage usage;
    check(getrusage(RUSAGE_SELF, &usage) == 0, "cannot read CPU usage");
    return (double)(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000 +
           (double)(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000;
}

static uint64_t resident_bytes(void) {
#ifdef __APPLE__
    struct mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    check(task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) ==
              KERN_SUCCESS,
          "cannot read resident memory");
    return info.resident_size;
#elif defined(__linux__)
    FILE *file = fopen("/proc/self/statm", "r");
    unsigned long total, resident;
    check(file && fscanf(file, "%lu %lu", &total, &resident) == 2, "cannot read resident memory");
    check(fclose(file) == 0, "cannot close memory snapshot");
    long page = sysconf(_SC_PAGESIZE);
    check(page > 0, "cannot read page size");
    return (uint64_t)resident * (uint64_t)page;
#else
    return 0;
#endif
}

static unsigned threads(void) {
#ifdef __APPLE__
    thread_act_array_t list;
    mach_msg_type_number_t count;
    check(task_threads(mach_task_self(), &list, &count) == KERN_SUCCESS, "cannot count threads");
    for (mach_msg_type_number_t i = 0; i < count; i++)
        check(mach_port_deallocate(mach_task_self(), list[i]) == KERN_SUCCESS,
              "cannot release thread port");
    check(vm_deallocate(mach_task_self(), (vm_address_t)list, count * sizeof(*list)) ==
              KERN_SUCCESS,
          "cannot release thread snapshot");
    return count;
#elif defined(__linux__)
    DIR *directory = opendir("/proc/self/task");
    check(directory != NULL, "cannot count threads");
    unsigned count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)))
        count += entry->d_name[0] >= '0' && entry->d_name[0] <= '9';
    check(closedir(directory) == 0, "cannot close thread snapshot");
    return count;
#else
    return 0;
#endif
}
#endif

static ah_code model(void *context, const char *request, ah_delta_sink delta, void *delta_context,
                     ah_cancel_check cancelled, void *cancel_context, char **out, ah_error *error) {
    (void)delta;
    (void)delta_context;
    (void)error;
    fixture *f = context;
    check(!cancelled(cancel_context), "model unexpectedly cancelled");
    f->models++;
    yyjson_doc *doc = yyjson_read(request, strlen(request), 0);
    check(doc != NULL, "invalid model request");
    yyjson_val *input = yyjson_obj_get(yyjson_doc_get_root(doc), "input"), *item;
    size_t index, count;
    unsigned receipts = 0;
    yyjson_arr_foreach(input, index, count, item) {
        if (!yyjson_equals_str(yyjson_obj_get(item, "type"), "function_call_output"))
            continue;
        check(yyjson_equals_str(yyjson_obj_get(item, "call_id"), "unit"), "wrong receipt ID");
        const char *json = yyjson_get_str(yyjson_obj_get(item, "output"));
        yyjson_doc *output = json ? yyjson_read(json, strlen(json), 0) : NULL;
        check(output != NULL, "missing tool receipt");
        yyjson_val *root = yyjson_doc_get_root(output);
        check(yyjson_is_false(yyjson_obj_get(root, "is_error")) &&
                  yyjson_get_uint(yyjson_obj_get(yyjson_obj_get(root, "result"), "value")) == 42,
              "incorrect tool receipt");
        yyjson_doc_free(output);
        receipts++;
    }
    check(receipts <= 1, "duplicate tool receipt");
    *out = strdup(receipts ? "{\"status\":\"completed\",\"output\":[]}"
                           : "{\"status\":\"completed\",\"output\":[{\"type\":\"function_call\","
                             "\"name\":\"unit\",\"call_id\":\"unit\",\"arguments\":\"{}\"}]}");
    check(*out != NULL, "cannot allocate model response");
    yyjson_doc_free(doc);
    return AH_OK;
}

static ah_code execute(void *context, const ah_tool_call *call, ah_tool_result *result,
                       ah_error *error) {
    (void)error;
    fixture *f = context;
    atomic_fetch_add(&f->callbacks, 1);
#ifndef AH_BENCH_ORDINARY_ONLY
    if (f->park && !call->wake_json)
        result->continuation_json =
            strdup("{\"checkpoint\":{\"phase\":1},\"wait\":{\"signal\":\"resume\"}}");
    else
#else
    (void)call;
#endif
        result->json = strdup("{\"value\":42}");
#ifndef AH_BENCH_ORDINARY_ONLY
    check(result->json || result->continuation_json, "cannot allocate tool result");
#else
    check(result->json != NULL, "cannot allocate tool result");
#endif
    return AH_OK;
}

static void observe(void *context, const ah_event *event) {
    fixture *f = context;
    f->receipts += !strcmp(event->type, "tool.completed");
}

static ah_engine *open_engine(fixture *f, bool durable) {
    ah_config config;
    ah_config_init(&config);
    config.database_path = ":memory:";
    config.provider = (ah_provider){model, f};
    config.observer = observe;
    config.observer_context = f;
    config.max_parallel_tools = 1;
    config.max_tool_calls = 2 * PARKED + OPERATIONS;
    ah_error error = {0};
    ah_engine *engine = NULL;
    ok(ah_open(&config, &engine, &error), &error);
#ifndef AH_BENCH_ORDINARY_ONLY
    ah_work_config work = {.version = 1, .replay = AH_REPLAY_SAFE};
#else
    check(!durable, "legacy comparison supports ordinary mode only");
#endif
    ah_tool tool = {
        .name = "unit",
        .description = "Deterministic benchmark operation",
        .parameters_json = "{\"type\":\"object\"}",
        .effect = AH_EFFECT_READ,
        .execute = execute,
        .context = f,
#ifndef AH_BENCH_ORDINARY_ONLY
        .work = durable ? &work : NULL,
#endif
    };
    ok(f->park ? ah_register_background_tool(engine, &tool, &error)
               : ah_register_tool(engine, &tool, &error),
       &error);
    return engine;
}

static int compare(const void *left, const void *right) {
    double a = *(const double *)left, b = *(const double *)right;
    return (a > b) - (a < b);
}

static void ordinary_overhead(bool durable) {
    double samples[SAMPLES], sorted[SAMPLES];
    for (unsigned run = 0; run < SAMPLES + WARMUPS; run++) {
        fixture f = {0};
        ah_engine *engine = open_engine(&f, durable);
        ah_error error = {0};
        double began = now_ms();
        for (unsigned i = 0; i < OPERATIONS; i++) {
            char session[AH_ID_CAP];
            ok(ah_session_create(engine, "mock", "", session, &error), &error);
            ok(ah_submit(engine, session, "run", "Execute one operation.", &error), &error);
            ok(ah_run(engine, session, &error), &error);
            ah_state state;
            ok(ah_session_state(engine, session, &state, &error), &error);
            check(state == AH_COMPLETED, "operation did not complete");
        }
        double elapsed = now_ms() - began;
        check(atomic_load(&f.callbacks) == OPERATIONS && f.models == 2 * OPERATIONS &&
                  f.receipts == OPERATIONS,
              "incorrect callback or receipt count");
        ah_close(engine);
        if (run >= WARMUPS)
            samples[run - WARMUPS] = elapsed;
    }
    memcpy(sorted, samples, sizeof(sorted));
    qsort(sorted, SAMPLES, sizeof(*sorted), compare);
    printf("{\"workload\":\"one_step_sessions\",\"mode\":\"%s\",\"store\":\"memory\","
           "\"operations_per_run\":%u,\"runs\":%u,\"warmups\":%u,"
           "\"timed\":\"create+submit+run+state\",\"network\":false,\"model_inference\":false,"
           "\"callbacks_and_receipts_checked\":true,\"p50_ms\":%.3f,\"raw_ms\":[",
           durable ? "durable" : "ordinary", OPERATIONS, SAMPLES, WARMUPS, sorted[SAMPLES / 2]);
    for (unsigned i = 0; i < SAMPLES; i++)
        printf("%s%.3f", i ? "," : "", samples[i]);
    puts("]}");
}

#ifndef AH_BENCH_ORDINARY_ONLY
static void parked_resources(bool batch_signals) {
    fixture f = {.park = true};
    ah_engine *engine = open_engine(&f, true);
    char session[AH_ID_CAP], operations[PARKED][AH_ID_CAP];
    ah_error error = {0};
    uint64_t before_bytes = resident_bytes();
    unsigned before_threads = threads();
    double began = now_ms();
    ok(ah_session_create(engine, "mock", "", session, &error), &error);
    for (unsigned i = 0; i < PARKED; i++) {
        char key[32];
        snprintf(key, sizeof(key), "park_%u", i);
        ok(ah_work_start(engine, session, key, "unit", "{}", operations[i], &error), &error);
        ok(ah_work_run(engine, session, &error), &error);
    }
    double parked_ms = now_ms() - began;
    check(atomic_load(&f.callbacks) == PARKED && !f.models && !f.receipts,
          "parked work unexpectedly executed or published");
    uint64_t parked_bytes = resident_bytes();
    unsigned parked_threads = threads();
    check(!before_threads || parked_threads == before_threads,
          "parked operations retained workers");
    double idle_cpu = cpu_ms(), idle_wall = now_ms();
    struct timespec pause = {.tv_nsec = 50000000};
    while (nanosleep(&pause, &pause) < 0)
        check(errno == EINTR, "cannot measure idle interval");
    idle_cpu = cpu_ms() - idle_cpu;
    idle_wall = now_ms() - idle_wall;
    began = now_ms();
    for (unsigned i = 0; i < PARKED; i++) {
        ah_signal_result delivery;
        ok(ah_signal(engine, session, operations[i], "resume", "true", &delivery, &error), &error);
        check(delivery == AH_SIGNAL_DELIVERED, "resume signal was not accepted");
        if (!batch_signals)
            ok(ah_work_run(engine, session, &error), &error);
    }
    if (batch_signals)
        ok(ah_work_run(engine, session, &error), &error);
    double resumed_ms = now_ms() - began;
    check(atomic_load(&f.callbacks) == 2 * PARKED && !f.models && f.receipts == PARKED,
          "resumed work had incorrect callback or receipt count");
    for (unsigned i = 0; i < PARKED; i++) {
        char *json = NULL;
        ok(ah_work_json(engine, session, operations[i], &json, &error), &error);
        yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
        check(doc != NULL, "cannot inspect final work receipt");
        yyjson_val *root = yyjson_doc_get_root(doc);
        check(yyjson_equals_str(yyjson_obj_get(root, "status"), "done") &&
                  yyjson_is_false(yyjson_obj_get(root, "is_error")) &&
                  yyjson_get_uint(yyjson_obj_get(yyjson_obj_get(root, "result"), "value")) == 42,
              "incorrect resumed work receipt");
        yyjson_doc_free(doc);
        ah_free(json);
    }
    printf("{\"workload\":\"parked_operations\",\"store\":\"memory\",\"operations\":%u,"
           "\"sessions\":1,\"worker_limit\":1,\"network\":false,\"model_inference\":false,"
           "\"timed_park\":\"create+start_and_run_each\",\"timed_resume\":\"%s\","
           "\"park_ms\":%.3f,\"resume_ms\":%.3f,\"resident_bytes_before\":%llu,"
           "\"resident_bytes_parked\":%llu,\"threads_before\":%u,\"threads_parked\":%u,"
           "\"idle_wall_ms\":%.3f,\"idle_process_cpu_ms\":%.3f,"
           "\"callbacks_and_receipts_checked\":true}\n",
           PARKED, batch_signals ? "all_signals_then_one_run" : "signal_and_run_each", parked_ms,
           resumed_ms, (unsigned long long)before_bytes, (unsigned long long)parked_bytes,
           before_threads, parked_threads, idle_wall, idle_cpu);
    ah_close(engine);
}
#endif

int main(void) {
    ordinary_overhead(false);
#ifndef AH_BENCH_ORDINARY_ONLY
    ordinary_overhead(true);
    parked_resources(false);
    parked_resources(true);
#endif
    return 0;
}
