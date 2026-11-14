#include "core/core.h"
#include "ah/content.h"
#include <assert.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static void ok(ah_code rc, const ah_error *err) {
    if (rc != AH_OK) {
        fprintf(stderr, "%s: %s\n", ah_code_name(rc), err->message);
        abort();
    }
}
static ah_code complete(void *ctx, const char *request, ah_delta_sink delta, void *dc,
                        ah_cancel_check cancel, void *cc, char **response, ah_error *err) {
    (void)ctx;
    (void)request;
    (void)delta;
    (void)dc;
    (void)cancel;
    (void)cc;
    (void)err;
    *response = ah_strdup("{\"status\":\"completed\",\"output\":[]}");
    return AH_OK;
}
static ah_engine *open_engine(const char *path) {
    ah_config config;
    ah_config_init(&config);
    config.database_path = path;
    config.provider.complete = complete;
    ah_engine *e = NULL;
    ah_error err = {0};
    ok(ah_open(&config, &e, &err), &err);
    return e;
}
static int scalar(ah_engine *e, const char *sql) {
    sqlite3_stmt *s = NULL;
    assert(sqlite3_prepare_v2(e->db, sql, -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    int value = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return value;
}
static void exec(ah_engine *e, const char *sql) {
    ah_error err = {0};
    ok(core_sql(e, sql, &err), &err);
}
static void session(ah_engine *e, char id[AH_ID_CAP]) {
    ah_error err = {0};
    ok(ah_session_create(e, "mock", "", id, &err), &err);
}
static void add_operation(ah_engine *e, const char *sid, char operation[AH_ID_CAP]) {
    ah_error err = {0};
    ok(core_id(operation, &err), &err);
    sqlite3_stmt *s = NULL;
    ok(core_prepare(
           e,
           "INSERT INTO "
           "operations(id,session_id,call_id,name,arguments,effect,approval,status,result) "
           "VALUES(?,?,'test','test','{}',0,0,'done','{}')",
           &s, &err),
       &err);
    sqlite3_bind_text(s, 1, operation, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, sid, -1, SQLITE_TRANSIENT);
    ok(core_done(e, s, &err), &err);
    ok(core_prepare(e, "INSERT INTO result_refs(session_id,operation_id) VALUES(?,?)", &s, &err),
       &err);
    sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, operation, -1, SQLITE_TRANSIENT);
    ok(core_done(e, s, &err), &err);
}
static void shared_receipts(void) {
    ah_engine *e = open_engine(":memory:");
    ah_error err = {0};
    char sid[AH_ID_CAP], fork[AH_ID_CAP], operation[AH_ID_CAP];
    session(e, sid);
    add_operation(e, sid, operation);
    ok(ah_fork(e, sid, fork, &err), &err);
    int events = scalar(e, "SELECT count(*) FROM events");
    assert(ah_forget(e, sid, false, &err) == AH_BUSY);
    assert(strstr(err.message, "dependent forks"));
    assert(strstr(err.message, fork));
    assert(scalar(e, "SELECT count(*) FROM sessions") == 2);
    assert(scalar(e, "SELECT count(*) FROM events") == events);
    char arguments[128];
    snprintf(arguments, sizeof(arguments), "{\"operation_id\":\"%s\"}", operation);
    ah_tool_call call = {.session_id = fork, .arguments_json = arguments, .output_limit = 4096};
    ah_tool_result result = {0};
    ok(core_read_result(e, &call, &result, &err), &err);
    free(result.json);
    ok(ah_forget(e, fork, false, &err), &err);
    assert(scalar(e, "SELECT count(*) FROM operations") == 1);
    ok(ah_forget(e, sid, false, &err), &err);
    assert(scalar(e, "SELECT count(*) FROM operations") == 0);
    assert(scalar(e, "SELECT count(*) FROM events") == 0);
    assert(scalar(e, "SELECT count(*) FROM pragma_foreign_key_check") == 0);
    ah_close(e);
}
static void protected_boundaries(void) {
    ah_engine *e = open_engine(":memory:");
    ah_error err = {0};
    char sid[AH_ID_CAP], operation[AH_ID_CAP];
    session(e, sid);
    ah_state states[] = {AH_READY,         AH_RUNNING,         AH_WAITING_APPROVAL,
                         AH_WAITING_INPUT, AH_OUTCOME_UNKNOWN, AH_WAITING_AGENTS};
    for (size_t i = 0; i < sizeof(states) / sizeof(*states); i++) {
        ok(core_state(e, sid, states[i], &err), &err);
        assert(ah_forget(e, sid, false, &err) == AH_BUSY);
    }
    ok(core_state(e, sid, AH_IDLE, &err), &err);
    add_operation(e, sid, operation);
    const char *pending[] = {"pending", "executing", "unknown", "agent_wait"};
    for (size_t i = 0; i < sizeof(pending) / sizeof(*pending); i++) {
        ok(core_set_operation(e, sid, operation, pending[i], &err), &err);
        assert(ah_forget(e, sid, false, &err) == AH_BUSY);
    }
    ok(core_set_operation(e, sid, operation, "done", &err), &err);
    exec(e, "INSERT INTO "
            "tasks(session_id,contract,revision,verifier_revision,model_limit,tool_limit,repair_"
            "limit,timeout_ms,deadline_ms,state) "
            "SELECT id,'{}','r','v',1,1,0,1,0,'unknown' FROM sessions");
    assert(ah_forget(e, sid, false, &err) == AH_BUSY);
    exec(e, "UPDATE tasks SET state='verifying'");
    assert(ah_forget(e, sid, false, &err) == AH_BUSY);
    exec(e, "UPDATE tasks SET state='active'");
    ok(core_state(e, sid, AH_FAILED, &err), &err);
    exec(e, "CREATE TABLE future_guard(session_id TEXT REFERENCES sessions(id)); "
            "INSERT INTO future_guard SELECT id FROM sessions");
    int events = scalar(e, "SELECT count(*) FROM events");
    assert(ah_forget(e, sid, false, &err) == AH_IO);
    assert(scalar(e, "SELECT count(*) FROM events") == events);
    assert(scalar(e, "SELECT count(*) FROM operations") == 1);
    exec(e, "DROP TABLE future_guard");
    ok(ah_forget(e, sid, false, &err), &err);
    ah_close(e);
}
static void shared_context(void) {
    ah_engine *e = open_engine(":memory:");
    ah_error err = {0};
    char sid[AH_ID_CAP], fork[AH_ID_CAP];
    session(e, sid);
    ok(core_add_item(e, sid, 0, "{\"role\":\"user\",\"content\":\"history\"}", &err), &err);
    exec(e, "INSERT INTO context_checkpoints(id,session_id,summary,before_bytes,after_bytes) "
            "SELECT 'checkpoint',id,'summary',100,10 FROM sessions;"
            "INSERT INTO context_archive_items(checkpoint_id,item_seq) SELECT 'checkpoint',seq "
            "FROM items;"
            "INSERT INTO context_checkpoint_refs(session_id,checkpoint_id) SELECT id,'checkpoint' "
            "FROM sessions");
    ok(ah_fork(e, sid, fork, &err), &err);
    assert(ah_forget(e, sid, false, &err) == AH_BUSY);
    assert(strstr(err.message, "context checkpoints"));
    ok(ah_forget(e, fork, false, &err), &err);
    assert(scalar(e, "SELECT count(*) FROM context_archive_items") == 1);
    ok(ah_forget(e, sid, false, &err), &err);
    assert(scalar(e, "SELECT count(*) FROM context_checkpoints") == 0);
    assert(scalar(e, "SELECT count(*) FROM pragma_foreign_key_check") == 0);
    ah_close(e);
}
static void agent_tree(void) {
    ah_engine *e = open_engine(":memory:");
    ah_error err = {0};
    char root[AH_ID_CAP], child[AH_ID_CAP];
    session(e, root);
    session(e, child);
    sqlite3_stmt *s = NULL;
    ok(core_prepare(e,
                    "INSERT INTO agent_nodes(session_id,root_id,parent_id,name,depth) VALUES"
                    "(?1,?1,NULL,'root',0),(?2,?1,?1,'child',1)",
                    &s, &err),
       &err);
    sqlite3_bind_text(s, 1, root, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, child, -1, SQLITE_TRANSIENT);
    ok(core_done(e, s, &err), &err);
    exec(e, "INSERT INTO agent_trees(root_id) SELECT session_id FROM agent_nodes WHERE depth=0;"
            "INSERT INTO agent_messages(sender,recipient,message_id,text,wake) "
            "SELECT parent_id,session_id,'m','task',1 FROM agent_nodes WHERE depth=1");
    assert(ah_forget(e, child, true, &err) == AH_BUSY);
    assert(ah_forget(e, root, false, &err) == AH_BUSY);
    assert(ah_forget(e, root, true, &err) == AH_BUSY);
    exec(e, "UPDATE agent_messages SET consumed=1");
    ok(core_state(e, child, AH_OUTCOME_UNKNOWN, &err), &err);
    assert(ah_forget(e, root, true, &err) == AH_BUSY);
    ok(core_state(e, child, AH_STOPPED, &err), &err);
    exec(e, "UPDATE agent_nodes SET resume_pending=1");
    ok(ah_forget(e, root, true, &err), &err);
    assert(scalar(e, "SELECT count(*) FROM sessions") == 0);
    assert(scalar(e, "SELECT count(*) FROM agent_messages") == 0);
    assert(scalar(e, "SELECT count(*) FROM pragma_foreign_key_check") == 0);
    ah_close(e);
}
static void artifacts_and_space(void) {
    char database[] = "/tmp/ah-retention-db-XXXXXX", path[] = "/tmp/ah-retention-file-XXXXXX";
    int dbfd = mkstemp(database), fd = mkstemp(path);
    assert(dbfd >= 0 && fd >= 0);
    close(dbfd);
    char data[4096];
    memset(data, 'a', sizeof(data));
    for (unsigned i = 0; i < 64; i++)
        assert(write(fd, data, sizeof(data)) == sizeof(data));
    close(fd);
    ah_engine *e = open_engine(database);
    ah_error err = {0};
    char sid[AH_ID_CAP], fork[AH_ID_CAP], artifact[AH_ARTIFACT_ID_CAP];
    session(e, sid);
    ok(ah_artifact_import(e, sid, path, "text/plain", artifact, &err), &err);
    ok(ah_fork(e, sid, fork, &err), &err);
    int pages = scalar(e, "PRAGMA page_count");
    ok(ah_forget(e, sid, false, &err), &err);
    assert(scalar(e, "SELECT count(*) FROM artifacts") == 1);
    char *catalog = NULL;
    ok(ah_artifacts_json(e, fork, &catalog, &err), &err);
    assert(strstr(catalog, artifact));
    free(catalog);
    ok(ah_forget(e, fork, false, &err), &err);
    assert(scalar(e, "SELECT count(*) FROM artifacts") == 0);
    assert(scalar(e, "PRAGMA freelist_count") > 0);
    ok(ah_storage_vacuum(e, &err), &err);
    assert(scalar(e, "PRAGMA page_count") < pages);
    ah_close(e);
    e = open_engine(database);
    assert(scalar(e, "SELECT count(*) FROM sessions") == 0);
    assert(scalar(e, "SELECT count(*) FROM artifacts") == 0);
    ah_close(e);
    char lock[sizeof(database) + 6];
    snprintf(lock, sizeof(lock), "%s.lock", database);
    assert(!unlink(path) && !unlink(database) && !unlink(lock));
}
int main(void) {
    shared_receipts();
    shared_context();
    protected_boundaries();
    agent_tree();
    artifacts_and_space();
    puts("retention tests passed");
    return 0;
}
