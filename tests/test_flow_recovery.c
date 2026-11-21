#include "core/flow_vm.h"
#include <signal.h>
#include <ah/sdk.h>
#include <assert.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
static void ok(ah_code code, const ah_error *error) {
    if (code != AH_OK) {
        fprintf(stderr, "%s: %s\n", ah_code_name(code), error->message);
        abort();
    }
}
static void crash(void *ctx, const ah_event *event) {
    (void)ctx;
    if (!strcmp(event->type, "tool.checkpoint") && strstr(event->json, "file.applied"))
        _exit(77);
}
static ah_sdk *open_sdk(const char *root, const char *db, ah_observer observer, void *context) {
    ah_sdk_config c;
    ah_sdk_config_init(&c);
    c.engine.database_path = db;
    c.engine.observer = observer;
    c.engine.observer_context = context;
    c.local.workspace = root;
    c.local.approve_writes = false;
    ah_sdk *sdk = NULL;
    ah_error error = {0};
    ok(ah_sdk_open(&c, &sdk, &error), &error);
    return sdk;
}

static char *one(ah_engine *e, const char *sql) {
    sqlite3_stmt *s = NULL;
    assert(sqlite3_prepare_v2(e->db, sql, -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    char *result = ah_strdup((const char *)sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
    return result;
}

typedef struct {
    ah_engine *engine;
    pid_t pid;
    unsigned waves;
    bool kill_vm, cancel;
} flow_probe;

static void watch_waves(void *context, const ah_event *event) {
    flow_probe *probe = context;
    if (strcmp(event->type, "work.parked"))
        return;
    core_flow_cell *cell = probe->engine->flow_cells;
    assert(cell && !strcmp(cell->operation, event->operation_id));
    ++probe->waves;
    if (probe->waves == 1)
        probe->pid = cell->pid;
    if (probe->waves <= 3 || !probe->kill_vm)
        assert(probe->pid == cell->pid);
    else
        assert(probe->pid != cell->pid);
    if (probe->waves == 3 && probe->kill_vm)
        assert(kill(cell->pid, SIGKILL) == 0);
    if (probe->waves == 3 && probe->cancel)
        ah_cancel(probe->engine);
}

static void live_waves(const char *root, bool kill_vm, bool cancel) {
    flow_probe probe = {.kill_vm = kill_vm, .cancel = cancel};
    ah_sdk *sdk = open_sdk(root, ":memory:", watch_waves, &probe);
    ah_engine *e = probe.engine = ah_sdk_engine(sdk);
    char sid[AH_ID_CAP], id[AH_ID_CAP], path[256];
    ah_error error = {0};
    snprintf(path, sizeof path, "%s/live", root);
    FILE *f = fopen(path, "w");
    assert(f && fputs("before", f) >= 0 && !fclose(f));
    ok(ah_session_create(e, "offline", "", sid, &error), &error);
    const char *flow =
        "{\"script\":\"const read=()=>tools.read_file({path:'live'});const a=await read();"
        "await tools.apply_changes({files:[{path:'live',base_revision:a.revision,"
        "edits:[{old_text:'before',new_text:'after'}]}]});"
        "const b=await Promise.all([read(),read()]);const c=await read();"
        "return [a.content,b[0].content,b[1].content,c.content];\"}";
    ok(ah_work_start(e, sid, "root", "flow", flow, id, &error), &error);
    ah_code status = ah_work_run(e, sid, &error);
    if (cancel)
        assert(status == AH_CANCELLED && probe.waves == 3);
    else {
        ok(status, &error);
        assert(probe.waves == 4);
        char *snapshot = NULL;
        ok(ah_work_json(e, sid, id, &snapshot, &error), &error);
        yyjson_doc *doc = yyjson_read(snapshot, strlen(snapshot), 0);
        yyjson_val *wake = yyjson_obj_get(yyjson_doc_get_root(doc), "wake");
        yyjson_val *last = yyjson_obj_get(yyjson_obj_get(wake, "calls"), "c4");
        assert(yyjson_equals_str(yyjson_obj_get(wake, "reason"), "calls"));
        assert(
            yyjson_equals_str(yyjson_obj_get(yyjson_obj_get(last, "result"), "content"), "after"));
        yyjson_doc_free(doc);
        free(snapshot);
        char *v = one(e, "SELECT result FROM operations WHERE name='flow' AND status='done'");
        assert(!strcmp(v, "[\"before\",\"after\",\"after\",\"after\"]"));
        free(v);
    }
    assert(!e->flow_cells);
    char *n = one(e, "SELECT COUNT(*) FROM events WHERE type='tool.started' AND operation_id IN "
                     "(SELECT id FROM operations WHERE name='apply_changes')");
    assert(!strcmp(n, "1"));
    free(n);
    n = one(e, "SELECT COUNT(*) FROM operations WHERE status='executing'");
    assert(!strcmp(n, "0"));
    free(n);
    ah_sdk_close(sdk);
    assert(!unlink(path));
}

static void deadline(const char *root) {
    ah_sdk *sdk = open_sdk(root, ":memory:", NULL, NULL);
    ah_engine *e = ah_sdk_engine(sdk);
    e->config.tool_timeout_ms = 100;
    char sid[AH_ID_CAP], id[AH_ID_CAP];
    ah_error error = {0};
    ok(ah_session_create(e, "offline", "", sid, &error), &error);
    ok(ah_work_start(e, sid, "root", "flow", "{\"script\":\"for (;;) {}\"}", id, &error), &error);
    uint64_t start = ah_now_ms();
    assert(ah_work_run(e, sid, &error) == AH_LIMIT);
    assert(ah_now_ms() - start < 4000 && !e->flow_cells);
    ah_sdk_close(sdk);
}

int main(int argc, char **argv) {
    if (argc == 4) {
        ah_sdk *sdk = open_sdk(argv[1], argv[2], crash, NULL);
        ah_engine *e = ah_sdk_engine(sdk);
        char op[AH_ID_CAP];
        ah_error error = {0};
        const char *flow = "{\"script\":\"const x=await tools.read_file({path:'a'});"
                           "await tools.apply_changes({files:[{path:'a',base_revision:x.revision,"
                           "edits:[{old_text:'old',new_text:'new'}]}]});return 'finished';\"}";
        ok(ah_work_start(e, argv[3], "root", "flow", flow, op, &error), &error);
        ok(ah_work_run(e, argv[3], &error), &error);
        return 2;
    }
    char root[] = "/tmp/ah-flow-kill-XXXXXX", db[256], file[256], lock[272];
    assert(mkdtemp(root));
    snprintf(db, sizeof db, "%s/session.db", root);
    snprintf(lock, sizeof lock, "%s.lock", db);
    snprintf(file, sizeof file, "%s/a", root);
    FILE *f = fopen(file, "w");
    assert(f && fputs("old", f) >= 0 && !fclose(f));
    ah_sdk *sdk = open_sdk(root, db, NULL, NULL);
    char sid[AH_ID_CAP];
    ah_error error = {0};
    ok(ah_session_create(ah_sdk_engine(sdk), "offline", "", sid, &error), &error);
    ah_sdk_close(sdk);
    pid_t pid;
    char *args[] = {argv[0], root, db, sid, NULL};
    int status;
    assert(posix_spawn(&pid, argv[0], NULL, NULL, args, environ) == 0);
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 77);
    sdk = open_sdk(root, db, NULL, NULL);
    ah_engine *e = ah_sdk_engine(sdk);
    ah_state state;
    ok(ah_session_state(e, sid, &state, &error), &error);
    assert(state == AH_OUTCOME_UNKNOWN);
    char bytes[4] = {0};
    f = fopen(file, "r");
    assert(f && fread(bytes, 1, 3, f) == 3 && !fclose(f));
    assert(!strcmp(bytes, "new"));
    char *id = one(e, "SELECT id FROM operations WHERE name='apply_changes' AND status='unknown'");
    ok(ah_resolve(e, sid, id, "{\"verified\":true}", false, &error), &error);
    free(id);
    ok(ah_work_run(e, sid, &error), &error);
    char *value = one(e, "SELECT result FROM operations WHERE name='flow' AND status='done'");
    assert(!strcmp(value, "\"finished\""));
    free(value);
    value = one(e, "SELECT COUNT(*) FROM events WHERE type='tool.started' AND operation_id IN "
                   "(SELECT id FROM operations WHERE name='apply_changes')");
    assert(!strcmp(value, "1"));
    free(value);
    ah_sdk_close(sdk);
    live_waves(root, false, false);
    live_waves(root, true, false);
    live_waves(root, false, true);
    deadline(root);
    assert(!unlink(db) && !unlink(file) && !unlink(lock) && !rmdir(root));
    return 0;
}
