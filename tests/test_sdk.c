#include <ah/sdk.h>
#include "core/core.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

static const char ask[] = "{\"status\":\"completed\",\"output\":[{\"type\":\"function_call\","
                          "\"name\":\"ask\",\"call_id\":\"ask-1\","
                          "\"arguments\":\"{\\\"question\\\":\\\"Continue?\\\"}\"}]}";
static const char done[] = "{\"status\":\"completed\",\"output\":[]}";
typedef struct {
    ah_engine *engine;
    char operation[AH_ID_CAP];
    unsigned models, events;
} fixture;
static void ok(ah_code code, const ah_error *error) {
    if (code != AH_OK) {
        fprintf(stderr, "%s: %s\n", ah_code_name(code), error->message);
        abort();
    }
}
static void observe(void *context, const ah_event *event) {
    fixture *f = context;
    f->events++;
    if (!strcmp(event->type, "input.requested"))
        memcpy(f->operation, event->operation_id, AH_ID_CAP);
    ah_state state;
    ah_error error = {0};
    if (f->engine)
        assert(ah_session_state(f->engine, event->session_id, &state, &error) == AH_BUSY);
}
static ah_code complete(void *context, const char *request, ah_delta_sink sink, void *sink_context,
                        ah_cancel_check cancel, void *cancel_context, char **out, ah_error *error) {
    (void)sink;
    (void)sink_context;
    (void)error;
    fixture *f = context;
    assert(!cancel(cancel_context));
    assert(!strstr(request, "\"name\":\"exec\"") && !strstr(request, "\"name\":\"mcp\"") &&
           !strstr(request, "\"name\":\"skill\"") && !strstr(request, "\"name\":\"spawn\""));
    *out = ah_strdup(f->models++ ? done : ask);
    return *out ? AH_OK : AH_IO;
}
static int descriptors(void) {
    int count = 0;
    for (int fd = 0; fd < 256; fd++)
        count += fcntl(fd, F_GETFD) >= 0;
    return count;
}
static void host_lifecycle(void) {
    char root[] = "/tmp/ah-sdk-lifecycle-XXXXXX", database[256], lock[272];
    assert(mkdtemp(root));
    snprintf(database, sizeof(database), "%s/state.db", root);
    snprintf(lock, sizeof(lock), "%s.lock", database);
    ah_sdk_config config;
    ah_sdk_config_init(&config);
    assert(!config.agents.max_active && !config.local.enable_exec && config.local.approve_writes);
    assert(!config.automatic_context && !config.extensions.enable_skills &&
           !config.extensions.enable_mcp && !config.extensions.enable_plugins);
    fixture f = {0};
    config.engine.database_path = database;
    config.engine.provider = (ah_provider){complete, &f};
    config.engine.observer = observe;
    config.engine.observer_context = &f;
    config.local.workspace = root;
    ah_sdk *sdk = NULL;
    ah_error error = {0};
    ok(ah_sdk_open(&config, &sdk, &error), &error);
    f.engine = ah_sdk_engine(sdk);
    assert(ah_sdk_local_tools(sdk));
    assert(core_find_tool(f.engine, "view_image") && !core_find_tool(f.engine, "exec"));
    assert(core_find_tool(f.engine, "write_file")->tool.requires_approval);
    assert(ah_sdk_set_token(sdk, "ignored", &error) == AH_INVALID);
    char id[AH_ID_CAP];
    ok(ah_session_create(f.engine, "mock", "", id, &error), &error);
    ok(ah_submit(f.engine, id, "one", "Hello", &error), &error);
    ok(ah_run(f.engine, id, &error), &error);
    ah_state state;
    ok(ah_session_state(f.engine, id, &state, &error), &error);
    assert(state == AH_WAITING_INPUT && f.operation[0] && f.models == 1);
    ah_sdk_close(sdk);
    f.engine = NULL;
    ok(ah_sdk_open(&config, &sdk, &error), &error);
    f.engine = ah_sdk_engine(sdk);
    ok(ah_session_state(f.engine, id, &state, &error), &error);
    assert(state == AH_WAITING_INPUT && f.models == 1);
    ok(ah_reply(f.engine, id, f.operation, "true", &error), &error);
    ok(ah_run(f.engine, id, &error), &error);
    ok(ah_session_state(f.engine, id, &state, &error), &error);
    assert(state == AH_COMPLETED && f.models == 2 && f.events);
    assert(ah_session_state(f.engine, "missing", &state, &error) == AH_NOT_FOUND);
    ah_sdk_close(sdk);
    f.engine = NULL;
    config.engine.database_path = ":memory:";
    int before = descriptors();
    for (unsigned i = 0; i < 30; i++) {
        ok(ah_sdk_open(&config, &sdk, &error), &error);
        ah_sdk_close(sdk);
    }
    assert(descriptors() == before);
    config.agents.max_active = 1;
    assert(ah_sdk_open(&config, &sdk, &error) == AH_INVALID && !sdk);
    assert(descriptors() == before);
    config.agents.max_active = 0;
    config.check_count = 1;
    assert(ah_sdk_open(&config, &sdk, &error) == AH_INVALID && !sdk);
    assert(unlink(database) == 0 && unlink(lock) == 0 && rmdir(root) == 0);
}
static void http(const char *endpoint, const char *workspace, bool async) {
    ah_sdk_config config;
    ah_sdk_config_init(&config);
    config.engine.database_path = ":memory:";
    config.responses.endpoint = endpoint;
    config.responses.bearer_token = "initial";
    config.responses.allow_loopback_http = true;
    config.responses.timeout_ms = 5000;
    config.responses.max_retries = 0;
    config.local.workspace = workspace;
    config.agents.max_active = async ? 1 : 0;
    fixture f = {0};
    config.engine.observer = observe;
    config.engine.observer_context = &f;
    ah_sdk *sdk = NULL;
    ah_error error = {0};
    ok(ah_sdk_open(&config, &sdk, &error), &error);
    f.engine = ah_sdk_engine(sdk);
    char id[AH_ID_CAP];
    ok(ah_session_create(f.engine, "fixture", "", id, &error), &error);
    ok(ah_submit(f.engine, id, "one", "Inspect image.bin then ask me", &error), &error);
    ok(ah_run(f.engine, id, &error), &error);
    ah_state state;
    ok(ah_session_state(f.engine, id, &state, &error), &error);
    assert(state == AH_WAITING_INPUT && f.operation[0]);
    ok(ah_sdk_set_token(sdk, "renewed", &error), &error);
    ok(ah_reply(f.engine, id, f.operation, "true", &error), &error);
    ok(ah_run(f.engine, id, &error), &error);
    ok(ah_session_state(f.engine, id, &state, &error), &error);
    assert(state == AH_COMPLETED);
    sqlite3_stmt *row = NULL;
    ok(core_prepare(f.engine, "SELECT count(*) FROM artifacts", &row, &error), &error);
    assert(sqlite3_step(row) == SQLITE_ROW && sqlite3_column_int(row, 0) == 1);
    sqlite3_finalize(row);
    ah_sdk_close(sdk);
}
int main(int argc, char **argv) {
    if (argc == 4)
        http(argv[1], argv[2], !strcmp(argv[3], "async"));
    else {
        assert(argc == 1);
        host_lifecycle();
        ah_sdk_close(NULL);
        assert(!ah_sdk_engine(NULL) && !ah_sdk_local_tools(NULL));
    }
    return 0;
}
