#include "core/workspace_internal.h"
#include "agents/agents_internal.h"
#include <assert.h>
#include <stdio.h>

static sqlite3_int64 scalar(ah_engine *e, const char *sql) {
    sqlite3_stmt *s = NULL;
    assert(sqlite3_prepare_v2(e->db, sql, -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    sqlite3_int64 value = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return value;
}

static void spawn(ah_engine *e, const char *parent, const char *args, unsigned ordinal,
                  bool failed) {
    ah_error error;
    char id[32];
    snprintf(id, sizeof(id), "scope-spawn-%u", ordinal);
    core_operation op = {.id = id,
                         .call_id = id,
                         .name = "spawn",
                         .arguments = (char *)args,
                         .status = "pending",
                         .effect = AH_EFFECT_READ,
                         .schema = (char *)agents_tool_schema("spawn")};
    sqlite3_stmt *s = NULL;
    assert(core_prepare(e,
                        "INSERT INTO operations(id,session_id,call_id,name,arguments,schema,"
                        "effect,approval,status) VALUES(?1,?2,?1,'spawn',?3,?4,0,0,'pending')",
                        &s, &error) == AH_OK);
    sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, parent, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, args, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 4, op.schema, -1, SQLITE_TRANSIENT);
    assert(core_done(e, s, &error) == AH_OK);
    core_session session;
    assert(core_load(e, parent, &session, &error) == AH_OK);
    assert(agents_execute(e, &session, &op, &error) == AH_OK);
    core_session_free(&session);
    assert(core_prepare(e, "SELECT is_error FROM operations WHERE id=?", &s, &error) == AH_OK);
    sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
    assert(sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int(s, 0) == failed);
    sqlite3_finalize(s);
}

void workspace_spawn_cases(const ah_config *base, const char *workspace) {
    ah_config config = *base;
    config.database_path = ":memory:";
    ah_engine *e;
    ah_error error;
    assert(ah_open(&config, &e, &error) == AH_OK);
    assert(ah_workspace_enable(e, workspace, &error) == AH_OK);
    char parent[AH_ID_CAP];
    assert(ah_session_create(e, "fixture", "", parent, &error) == AH_OK);
    assert(agents_register_root(e, parent, &error) == AH_OK);
    ah_agents_config_init(&e->agents);
    e->agents_enabled = true;
    const char *paths[] = {"a.txt"};
    assert(ah_workspace_assign(e, parent, paths, 1, false, &error) == AH_OK);
    spawn(e, parent,
          "{\"name\":\"rejected\",\"message\":\"edit\","
          "\"write_paths\":[\"a.txt\",\"missing\"]}",
          1, true);
    assert(scalar(e, "SELECT count(*) FROM sessions") == 1);
    assert(scalar(e, "SELECT count(*) FROM agent_nodes") == 1);
    assert(scalar(e, "SELECT count(*) FROM workspace_grants") == 1);
    /* Duplicate transfers also roll back the first successful transfer. */
    spawn(e, parent,
          "{\"name\":\"duplicate\",\"message\":\"edit\","
          "\"write_paths\":[\"a.txt\",\"a.txt\"]}",
          2, true);
    assert(scalar(e, "SELECT count(*) FROM sessions") == 1);
    spawn(e, parent,
          "{\"name\":\"writer\",\"message\":\"edit\","
          "\"write_paths\":[\"a.txt\"]}",
          3, false);
    assert(scalar(e, "SELECT count(*) FROM sessions") == 2);
    assert(scalar(e, "SELECT count(*) FROM workspace_grants g JOIN agent_nodes n "
                     "ON n.session_id=g.session_id WHERE n.parent_id IS NOT NULL") == 1);
    spawn(e, parent, "{\"name\":\"reader\",\"message\":\"read only\"}", 4, false);
    assert(scalar(e, "SELECT count(*) FROM sessions") == 3);
    assert(scalar(e, "SELECT count(*) FROM workspace_grants") == 1);
    assert(ah_workspace_assign(e, parent, paths, 1, false, &error) == AH_BUSY);
    ah_close(e);
}
