#include "durable_fixture.h"
#include "ah/work.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>

extern char **environ;

void df_ok(ah_code code, const ah_error *error) {
    if (code != AH_OK) {
        fprintf(stderr, "%s: %s\n", ah_code_name(code), error->message);
        abort();
    }
}

static char *calls(durable_fixture *f) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    assert(doc);
    yyjson_mut_val *root = yyjson_mut_obj(doc), *output = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "status", "completed");
    yyjson_mut_obj_add_val(doc, root, "output", output);
    for (unsigned i = 0; i < f->batch; i++) {
        char id[32];
        snprintf(id, sizeof(id), "durable_%u", i);
        yyjson_mut_val *call = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, call, "type", "function_call");
        yyjson_mut_obj_add_str(doc, call, "namespace", "local");
        yyjson_mut_obj_add_str(doc, call, "name", f->tool);
        yyjson_mut_obj_add_strcpy(doc, call, "call_id", id);
        yyjson_mut_obj_add_str(doc, call, "arguments", f->arguments ? f->arguments : "{}");
        yyjson_mut_arr_append(output, call);
    }
    char *json = yyjson_mut_write(doc, 0, NULL);
    assert(json);
    yyjson_mut_doc_free(doc);
    return json;
}

static ah_code model(void *context, const char *request, ah_delta_sink delta, void *delta_context,
                     ah_cancel_check cancelled, void *cancel_context, char **out, ah_error *error) {
    (void)delta;
    (void)delta_context;
    (void)error;
    durable_fixture *f = context;
    assert(!cancelled(cancel_context));
    assert(pthread_equal(f->owner, pthread_self()));
    yyjson_doc *doc = yyjson_read(request, strlen(request), 0);
    assert(doc);
    yyjson_val *input = yyjson_obj_get(yyjson_doc_get_root(doc), "input"), *item;
    size_t i, count;
    unsigned receipts = 0;
    yyjson_arr_foreach(input, i, count, item) {
        if (!yyjson_equals_str(yyjson_obj_get(item, "type"), "function_call_output"))
            continue;
        char id[32];
        snprintf(id, sizeof(id), "durable_%u", receipts);
        assert(yyjson_equals_str(yyjson_obj_get(item, "call_id"), id));
        const char *json = yyjson_get_str(yyjson_obj_get(item, "output"));
        yyjson_doc *output = json ? yyjson_read(json, strlen(json), 0) : NULL;
        assert(output);
        yyjson_val *root = yyjson_doc_get_root(output);
        assert(yyjson_is_false(yyjson_obj_get(root, "is_error")));
        assert(yyjson_get_uint(yyjson_obj_get(yyjson_obj_get(root, "result"), "value")) == 7);
        yyjson_doc_free(output);
        receipts++;
    }
    assert(receipts == 0 || receipts == f->batch);
    f->receipts += receipts;
    f->models++;
    *out = receipts ? strdup("{\"status\":\"completed\",\"output\":[]}") : calls(f);
    assert(*out);
    yyjson_doc_free(doc);
    return AH_OK;
}

static void observe(void *context, const ah_event *event) {
    durable_fixture *f = context;
    assert(pthread_equal(f->owner, pthread_self()));
    if (!strcmp(event->type, "tool.started")) {
        assert(f->started < DF_OPERATIONS && event->operation_id);
        snprintf(f->operations[f->started++], AH_ID_CAP, "%s", event->operation_id);
        if (f->signal_on_start) {
            ah_error error = {0};
            ah_signal_result delivery;
            f->reentry_code = ah_signal(f->engine, event->session_id, event->operation_id, "ready",
                                        "true", &delivery, &error);
        }
    }
    if (f->crash_after_receipt && !strcmp(event->type, "tool.received"))
        df_ready(f->ready_fd);
}

void df_open(durable_fixture *f, const char *database, unsigned workers) {
    f->owner = pthread_self();
    if (!f->batch)
        f->batch = 1;
    if (!f->tool)
        f->tool = "durable";
    ah_config config;
    ah_config_init(&config);
    config.database_path = database;
    config.provider = (ah_provider){model, f};
    config.observer = observe;
    config.observer_context = f;
    config.max_parallel_tools = workers;
    config.run_timeout_ms = 5000;
    config.tool_timeout_ms = 3000;
    ah_error error = {0};
    df_ok(ah_open(&config, &f->engine, &error), &error);
}

void df_create(durable_fixture *f, char session[AH_ID_CAP]) {
    ah_error error = {0};
    df_ok(ah_session_create(f->engine, "mock", "", session, &error), &error);
    df_ok(ah_submit(f->engine, session, "input", "Run the durable fixture.", &error), &error);
}

void df_state(durable_fixture *f, const char *session, ah_state expected) {
    ah_error error = {0};
    ah_state actual;
    df_ok(ah_session_state(f->engine, session, &actual, &error), &error);
    if (actual != expected) {
        fprintf(stderr, "expected state %s, got %s\n", ah_state_name(expected),
                ah_state_name(actual));
        abort();
    }
}

typedef struct {
    const char *type;
    unsigned count;
} event_count;

static void count_event(void *context, const ah_event *event) {
    event_count *count = context;
    count->count += !strcmp(event->type, count->type);
}

unsigned df_events(durable_fixture *f, const char *session, const char *type) {
    event_count count = {.type = type};
    ah_error error = {0};
    df_ok(ah_events(f->engine, session, 0, count_event, &count, &error), &error);
    return count.count;
}

void df_database(char path[64]) {
    snprintf(path, 64, "/tmp/ah-durable-XXXXXX");
    int fd = mkstemp(path);
    assert(fd >= 0 && close(fd) == 0);
}

void df_remove_database(const char *path) {
    const char *suffixes[] = {"", ".lock", "-wal", "-shm"};
    for (unsigned i = 0; i < sizeof(suffixes) / sizeof(*suffixes); i++) {
        char name[128];
        snprintf(name, sizeof(name), "%s%s", path, suffixes[i]);
        assert(unlink(name) == 0 || errno == ENOENT);
    }
}

void df_ready(int fd) {
    assert(write(fd, "R", 1) == 1);
    for (;;)
        pause();
}

void df_kill_child(const char *executable, const char *mode, const char *database,
                   const char *session) {
    int ready[2];
    assert(pipe(ready) == 0);
    char fd[24];
    snprintf(fd, sizeof(fd), "%d", ready[1]);
    char *argv[] = {(char *)executable, (char *)mode, (char *)database, (char *)session, fd, NULL};
    posix_spawn_file_actions_t actions;
    assert(posix_spawn_file_actions_init(&actions) == 0);
    assert(posix_spawn_file_actions_addclose(&actions, ready[0]) == 0);
    pid_t pid;
    assert(posix_spawn(&pid, executable, &actions, NULL, argv, environ) == 0);
    assert(posix_spawn_file_actions_destroy(&actions) == 0 && close(ready[1]) == 0);
    struct pollfd waiting = {.fd = ready[0], .events = POLLIN};
    int available;
    do {
        available = poll(&waiting, 1, 5000);
    } while (available < 0 && errno == EINTR);
    char message = 0;
    bool observed = available == 1 && read(ready[0], &message, 1) == 1 && message == 'R';
    assert(close(ready[0]) == 0);
    int killed = kill(pid, SIGKILL);
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    assert(observed && killed == 0 && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
}

void df_sleep(unsigned milliseconds) {
    struct timespec pause = {.tv_sec = milliseconds / 1000,
                             .tv_nsec = (long)(milliseconds % 1000) * 1000000L};
    while (nanosleep(&pause, &pause) < 0)
        assert(errno == EINTR);
}
