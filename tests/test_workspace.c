#include "workflow_fixture.h"
#include "core/workspace_internal.h"
#include <stdio.h>

void workspace_spawn_cases(const ah_config *, const char *);
void workspace_alias_cases(const ah_config *);

static struct {
    const char *name, *arguments;
    unsigned step, turn;
} model;

static ah_code complete(void *context, const char *request, ah_delta_sink delta,
                        void *delta_context, ah_cancel_check cancelled, void *cancel_context,
                        char **response, ah_error *error) {
    (void)context;
    (void)request;
    (void)delta;
    (void)delta_context;
    (void)cancelled;
    (void)cancel_context;
    (void)error;
    if (model.step++) {
        *response = ah_strdup("{\"status\":\"completed\",\"output\":[{\"type\":\"message\","
                              "\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\","
                              "\"text\":\"done\"}]}]}");
        return AH_OK;
    }
    yyjson_mut_doc *d = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, root);
    yyjson_mut_obj_add_str(d, root, "status", "completed");
    yyjson_mut_val *output = yyjson_mut_obj_add_arr(d, root, "output"), *call = yyjson_mut_obj(d);
    yyjson_mut_arr_append(output, call);
    yyjson_mut_obj_add_str(d, call, "type", "function_call");
    yyjson_mut_obj_add_str(d, call, "name", model.name);
    char id[32];
    snprintf(id, sizeof(id), "call-%u", model.turn);
    yyjson_mut_obj_add_strcpy(d, call, "call_id", id);
    yyjson_mut_obj_add_str(d, call, "arguments", model.arguments);
    *response = ah_mut_write(d);
    ah_mut_doc_free(d);
    assert(*response);
    return AH_OK;
}

static void okay(ah_code code, ah_error *error) {
    if (code != AH_OK) {
        fprintf(stderr, "%s: %s\n", ah_code_name(code), error->message);
        abort();
    }
}

static void run(ah_engine *e, const char *sid, const char *name, const char *args, bool failed) {
    model.name = name;
    model.arguments = args;
    model.step = 0;
    char id[32];
    snprintf(id, sizeof(id), "input-%u", ++model.turn);
    ah_error error;
    okay(ah_submit(e, sid, id, "perform the fixture action", &error), &error);
    okay(ah_run(e, sid, &error), &error);
    sqlite3_stmt *s = NULL;
    okay(core_prepare(e,
                      "SELECT is_error,status FROM operations WHERE session_id=? "
                      "ORDER BY seq DESC LIMIT 1",
                      &s, &error),
         &error);
    sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(sqlite3_column_int(s, 0) == failed);
    assert(!strcmp((const char *)sqlite3_column_text(s, 1), "done"));
    sqlite3_finalize(s);
}

static void ownership(ah_engine *e, workflow *w, const char *a, const char *b) {
    ah_error error;
    const char *one[] = {"a.txt"}, *two[] = {"b.txt"}, *root[] = {"."};
    okay(ah_workspace_enable(e, w->workspace, &error), &error);
    okay(ah_workspace_assign(e, a, one, 1, false, &error), &error);
    assert(ah_workspace_assign(e, b, one, 1, false, &error) == AH_BUSY);
    assert(ah_workspace_assign(e, b, root, 1, false, &error) == AH_BUSY);
    okay(ah_workspace_assign(e, b, two, 1, false, &error), &error);
    assert(ah_workspace_assign(e, a, one, 1, true, &error) == AH_BUSY);
    const char *invalid[] = {"/a.txt", "./a.txt", "x/../a.txt", "x//a.txt", "x/", ""};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++)
        assert(ah_workspace_assign(e, a, &invalid[i], 1, false, &error) == AH_INVALID);
    char path[PATH_MAX];
    wf_path(w, "link", path);
    assert(symlink("a.txt", path) == 0);
    const char *link[] = {"link"};
    assert(ah_workspace_assign(e, a, link, 1, false, &error) == AH_INVALID);
    assert(unlink(path) == 0);
    wf_path(w, "directory", path);
    assert(mkdir(path, 0700) == 0);
    wf_path(w, "directory-link", path);
    assert(symlink("directory", path) == 0);
    const char *linked_parent[] = {"directory-link/new"};
    assert(ah_workspace_assign(e, a, linked_parent, 1, false, &error) == AH_INVALID);
    assert(unlink(path) == 0);
    const char *dir[] = {"directory"}, *inside[] = {"directory/new"}, *near[] = {"directory2"};
    okay(ah_workspace_assign(e, a, dir, 1, false, &error), &error);
    assert(ah_workspace_assign(e, b, inside, 1, false, &error) == AH_BUSY);
    okay(ah_workspace_assign(e, b, near, 1, false, &error), &error);
    assert(ah_workspace_assign(e, a, inside, 1, false, &error) == AH_OK);
    assert(ah_workspace_assign(e, b, dir, 1, false, &error) == AH_BUSY);
    okay(ah_workspace_assign(e, a, one, 1, false, &error), &error);
    okay(ah_workspace_assign(e, b, two, 1, false, &error), &error);
    /* An event failure rolls back replacement, retaining the old owner. */
    okay(core_sql(e,
                  "CREATE TEMP TRIGGER reject_grant BEFORE INSERT ON events "
                  "WHEN NEW.type='workspace.assigned' BEGIN SELECT RAISE(ABORT,'test'); END",
                  &error),
         &error);
    assert(ah_workspace_assign(e, a, NULL, 0, false, &error) == AH_INVALID);
    okay(core_sql(e, "DROP TRIGGER reject_grant", &error), &error);
    assert(ah_workspace_assign(e, b, one, 1, false, &error) == AH_BUSY);
}

static void revisions(ah_engine *e, workflow *w, const char *a, const char *b) {
    run(e, b, "write_file", "{\"path\":\"a.txt\",\"content\":\"wrong\",\"expect_absent\":true}",
        true);
    run(e, a, "write_file", "{\"path\":\"a.txt\",\"content\":\"wrong\"}", true);
    run(e, a, "write_file", "{\"path\":\"a.txt\",\"content\":\"wrong\",\"expect_absent\":true}",
        true);
    wf_expect_file(w, "a.txt", WF_BASE);
    run(e, a, "write_file",
        "{\"path\":\"a.txt\",\"content\":\"new\",\"base_revision\":\"" WF_REV "\"}", false);
    run(e, a, "write_file",
        "{\"path\":\"a.txt\",\"content\":\"stale\",\"base_revision\":\"" WF_REV "\"}", true);
    wf_expect_file(w, "a.txt", "new");
    wf_reset_changes(w);
    run(e, a, "apply_changes",
        "{\"files\":[{\"path\":\"a.txt\",\"edits\":[{\"old_text\":\"A\",\"new_text\":\"X\"}]}]}",
        true);
    char *args = wf_apply_args(NULL, false, NULL);
    run(e, a, "apply_changes", args, false);
    run(e, a, "apply_changes", args, true);
    free(args);
    wf_expect_file(w, "a.txt", "X\nB\nZ\nD\n");
    ah_error error;
    const char *fresh[] = {"fresh"};
    okay(ah_workspace_assign(e, a, fresh, 1, false, &error), &error);
    run(e, a, "write_file", "{\"path\":\"fresh\",\"content\":\"created\",\"expect_absent\":true}",
        false);
    run(e, a, "write_file", "{\"path\":\"fresh\",\"content\":\"clobber\",\"expect_absent\":true}",
        true);
    wf_expect_file(w, "fresh", "created");
    char fork[AH_ID_CAP];
    okay(ah_fork(e, a, fork, &error), &error);
    run(e, fork, "write_file", "{\"path\":\"fresh\",\"content\":\"forked\",\"expect_absent\":true}",
        true);
}

static void delegation(ah_engine *e, workflow *w, const char *a, const char *b) {
    ah_error error;
    const char *path[] = {"fresh"};
    assert(ah_workspace_transfer(e, a, b, "fresh", &error) == AH_AUTH);
    sqlite3_stmt *s = NULL;
    okay(core_prepare(
             e, "INSERT INTO agent_nodes(session_id,root_id,name,depth) VALUES(?1,?1,'root',0)", &s,
             &error),
         &error);
    sqlite3_bind_text(s, 1, a, -1, SQLITE_TRANSIENT);
    okay(core_done(e, s, &error), &error);
    okay(core_prepare(e,
                      "INSERT INTO agent_nodes(session_id,root_id,parent_id,name,depth) "
                      "VALUES(?1,?2,?2,'child',1)",
                      &s, &error),
         &error);
    sqlite3_bind_text(s, 1, b, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, a, -1, SQLITE_TRANSIENT);
    okay(core_done(e, s, &error), &error);
    okay(ah_workspace_transfer(e, a, b, "fresh", &error), &error);
    assert(ah_workspace_assign(e, a, path, 1, false, &error) == AH_BUSY);
    const char *stale_write =
        "{\"path\":\"fresh\",\"content\":\"child revision\",\"base_revision\":"
        "\"406effb1e9c59672c66a598c2b21e331b23b16c54024e96d6df3e7c173549791\"}";
    run(e, b, "write_file", stale_write, false);
    okay(ah_workspace_transfer(e, b, a, "fresh", &error), &error);
    run(e, a, "write_file", stale_write, true);
    wf_expect_file(w, "fresh", "child revision");
    okay(core_prepare(
             e,
             "INSERT INTO operations(id,session_id,call_id,name,arguments,effect,approval,status) "
             "VALUES('busy-op',?,'busy-call','exec','{}',2,0,'executing')",
             &s, &error),
         &error);
    sqlite3_bind_text(s, 1, a, -1, SQLITE_TRANSIENT);
    okay(core_done(e, s, &error), &error);
    assert(ah_workspace_transfer(e, a, b, "fresh", &error) == AH_BUSY);
    assert(ah_workspace_assign(e, a, NULL, 0, false, &error) == AH_BUSY);
    okay(core_sql(e, "UPDATE operations SET status='unknown' WHERE id='busy-op'", &error), &error);
    assert(ah_workspace_transfer(e, a, b, "fresh", &error) == AH_BUSY);
    assert(ah_workspace_assign(e, a, NULL, 0, false, &error) == AH_BUSY);
    okay(core_sql(e, "DELETE FROM operations WHERE id='busy-op'", &error), &error);
}

static void boundaries(ah_engine *e, workflow *w, const char *a, const char *b) {
    ah_error error;
    char other[] = "/tmp/ah-workspace-other-XXXXXX";
    assert(mkdtemp(other));
    assert(ah_workspace_enable(e, other, &error) == AH_AUTH);
    ah_local_tools *tools;
    ah_local_tools_config config = {.workspace = other};
    okay(ah_local_tools_open(&config, &tools, &error), &error);
    core_tool wrong = {.tool = {.execute = ah_local_write_file, .context = tools}};
    core_operation op = {.effect = AH_EFFECT_WRITE, .arguments = "{\"path\":\"fresh\"}"};
    bool coordinated = false;
    assert(core_workspace_authorize(e, a, &op, &wrong, &coordinated, &error) == AH_AUTH);
    assert(coordinated);
    ah_local_tools_close(tools);
    assert(rmdir(other) == 0);
    core_tool process = {.tool = {.execute = ah_local_exec, .context = w->tools}};
    op.arguments = "{\"argv\":[\"/bin/true\"]}";
    assert(core_workspace_authorize(e, a, &op, &process, &coordinated, &error) == AH_AUTH);
    okay(ah_workspace_assign(e, b, NULL, 0, false, &error), &error);
    okay(ah_workspace_assign(e, a, NULL, 0, true, &error), &error);
    okay(core_workspace_authorize(e, a, &op, &process, &coordinated, &error), &error);
    sqlite3_stmt *s = NULL;
    okay(core_prepare(e,
                      "INSERT INTO operations(id,session_id,call_id,name,arguments,effect,"
                      "approval,status) VALUES('barrier',?,'barrier','exec','{}',2,0,'executing')",
                      &s, &error),
         &error);
    sqlite3_bind_text(s, 1, b, -1, SQLITE_TRANSIENT);
    okay(core_done(e, s, &error), &error);
    assert(core_workspace_authorize(e, a, &op, &process, &coordinated, &error) == AH_BUSY);
    okay(core_sql(e, "UPDATE operations SET status='unknown' WHERE id='barrier'", &error), &error);
    assert(core_workspace_authorize(e, a, &op, &process, &coordinated, &error) == AH_AUTH);
    okay(core_sql(e, "DELETE FROM operations WHERE id='barrier'", &error), &error);
    const char *fresh[] = {"fresh"};
    assert(ah_workspace_assign(e, b, fresh, 1, false, &error) == AH_BUSY);
    okay(ah_workspace_assign(e, a, fresh, 1, false, &error), &error);
}

int main(int argc, char **argv) {
    (void)argc;
    workflow w;
    wf_open(&w, argv[0]);
    wf_reset_changes(&w);
    char database[PATH_MAX], a[AH_ID_CAP], b[AH_ID_CAP];
    wf_path(&w, "database", database);
    ah_config config;
    ah_config_init(&config);
    config.database_path = database;
    config.provider.complete = complete;
    ah_engine *e;
    ah_error error;
    okay(ah_open(&config, &e, &error), &error);
    okay(ah_local_tools_register(w.tools, e, &error), &error);
    okay(ah_session_create(e, "fixture", "", a, &error), &error);
    okay(ah_session_create(e, "fixture", "", b, &error), &error);
    ownership(e, &w, a, b);
    revisions(e, &w, a, b);
    delegation(e, &w, a, b);
    boundaries(e, &w, a, b);
    char *before = NULL, *after = NULL;
    okay(ah_workspace_json(e, &before, &error), &error);
    ah_close(e);
    okay(ah_open(&config, &e, &error), &error);
    okay(ah_workspace_json(e, &after, &error), &error);
    assert(!strcmp(before, after));
    free(before);
    free(after);
    const char *fresh[] = {"fresh"};
    assert(ah_workspace_assign(e, b, fresh, 1, false, &error) == AH_BUSY);
    okay(ah_workspace_assign(e, a, NULL, 0, false, &error), &error);
    okay(ah_workspace_assign(e, b, fresh, 1, false, &error), &error);
    ah_close(e);
    workspace_spawn_cases(&config, w.workspace);
    workspace_alias_cases(&config);
    wf_close(&w);
    return 0;
}
