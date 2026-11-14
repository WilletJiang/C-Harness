#include "core.h"
#include "ah/content.h"
#include <stdio.h>

static ah_code require_absent(ah_engine *e, const char *sql, const char *reason, ah_error *err) {
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(e, sql, &s, err);
    if (rc == AH_OK) {
        int step = sqlite3_step(s);
        if (step == SQLITE_ROW) {
            const char *dependent = sqlite3_column_type(s, 0) == SQLITE_TEXT
                                        ? (const char *)sqlite3_column_text(s, 0)
                                        : NULL;
            rc = ah_fail(err, AH_BUSY, "%s%s%s", reason, dependent ? "; dependent session: " : "",
                         dependent ? dependent : "");
        } else if (step != SQLITE_DONE)
            rc = ah_fail(err, AH_IO, "cannot inspect retention boundary");
    }
    sqlite3_finalize(s);
    return rc;
}
static ah_code retention_boundary(ah_engine *e, ah_error *err) {
    char state_query[192];
    snprintf(state_query, sizeof(state_query),
             "SELECT 1 FROM sessions WHERE id IN (SELECT id FROM forget_sessions) "
             "AND state NOT IN (%d,%d,%d,%d) LIMIT 1",
             AH_IDLE, AH_COMPLETED, AH_STOPPED, AH_FAILED);
    ah_code rc = require_absent(
        e, state_query, "forget requires quiescent sessions throughout the selected tree", err);
    if (rc == AH_OK)
        rc = require_absent(
            e,
            "SELECT 1 FROM operations WHERE session_id IN (SELECT id FROM forget_sessions) "
            "AND status!='done' LIMIT 1",
            "pending or uncertain operations prevent forgetting", err);
    if (rc == AH_OK)
        rc = require_absent(
            e,
            "SELECT 1 FROM tasks WHERE session_id IN (SELECT id FROM forget_sessions) "
            "AND state IN ('unknown','verifying') LIMIT 1",
            "task verification must be reconciled before forgetting", err);
    if (rc == AH_OK)
        rc = require_absent(e,
                            "SELECT 1 FROM agent_messages WHERE consumed=0 AND "
                            "(sender IN (SELECT id FROM forget_sessions) OR recipient IN "
                            "(SELECT id FROM forget_sessions)) LIMIT 1",
                            "pending agent messages prevent forgetting", err);
    if (rc == AH_OK)
        rc = require_absent(
            e,
            "SELECT r.session_id FROM result_refs r JOIN operations o ON o.id=r.operation_id "
            "WHERE o.session_id IN (SELECT id FROM forget_sessions) AND r.session_id NOT IN "
            "(SELECT id FROM forget_sessions) LIMIT 1",
            "surviving sessions reference these receipts; forget dependent forks first", err);
    if (rc == AH_OK)
        rc = require_absent(
            e,
            "SELECT r.session_id FROM context_checkpoint_refs r JOIN context_checkpoints c ON "
            "c.id=r.checkpoint_id "
            "WHERE c.session_id IN (SELECT id FROM forget_sessions) AND r.session_id NOT IN "
            "(SELECT id FROM forget_sessions) LIMIT 1",
            "surviving sessions reference context checkpoints; forget dependent forks first", err);
    return rc;
}
/* Optional subsystems may be absent in old stores. Unknown foreign-key dependents
 * are deliberately not guessed: SQLite rejects the transaction without data loss. */
static ah_code delete_rows(ah_engine *e, const char *table, const char *predicate, ah_error *err) {
    sqlite3_stmt *s = NULL;
    ah_code rc =
        core_prepare(e, "SELECT 1 FROM sqlite_schema WHERE type='table' AND name=?", &s, err);
    int step = SQLITE_ERROR;
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, table, -1, SQLITE_STATIC);
        step = sqlite3_step(s);
    }
    sqlite3_finalize(s);
    if (rc != AH_OK || step == SQLITE_DONE)
        return rc;
    if (step != SQLITE_ROW)
        return ah_fail(err, AH_IO, "cannot inspect retention tables");
    char sql[512];
    int n = snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE %s", table, predicate);
    if (n < 0 || (size_t)n >= sizeof(sql))
        return ah_fail(err, AH_IO, "retention statement exceeds internal bound");
    return core_sql(e, sql, err);
}
static ah_code forget_rows(ah_engine *e, ah_error *err) {
    const char *by_session = "session_id IN (SELECT id FROM forget_sessions)";
    ah_code rc = delete_rows(e, "context_checkpoint_refs", by_session, err);
    if (rc == AH_OK)
        rc = delete_rows(e, "context_archive_items",
                         "checkpoint_id IN (SELECT id FROM context_checkpoints "
                         "WHERE session_id IN (SELECT id FROM forget_sessions))",
                         err);
    const char *tables[] = {"context_checkpoints",
                            "extension_context",
                            "workspace_owners",
                            "observations",
                            "result_refs",
                            "artifact_refs",
                            "tasks",
                            "agent_usage",
                            "call_ids",
                            "commands",
                            "events"};
    for (size_t i = 0; rc == AH_OK && i < sizeof(tables) / sizeof(*tables); i++)
        rc = delete_rows(e, tables[i], by_session, err);
    if (rc == AH_OK)
        rc = delete_rows(e, "agent_waits",
                         "operation_id IN (SELECT id FROM operations WHERE "
                         "session_id IN (SELECT id FROM forget_sessions))",
                         err);
    if (rc == AH_OK)
        rc = delete_rows(e, "agent_messages",
                         "sender IN (SELECT id FROM forget_sessions) OR "
                         "recipient IN (SELECT id FROM forget_sessions)",
                         err);
    if (rc == AH_OK)
        rc = delete_rows(e, "agent_nodes", by_session, err);
    if (rc == AH_OK)
        rc = delete_rows(e, "agent_trees", "root_id IN (SELECT id FROM forget_sessions)", err);
    if (rc == AH_OK)
        rc = delete_rows(e, "items", by_session, err);
    if (rc == AH_OK)
        rc = delete_rows(e, "operations", by_session, err);
    if (rc == AH_OK)
        rc = delete_rows(e, "sessions", "id IN (SELECT id FROM forget_sessions)", err);
    if (rc == AH_OK)
        rc = core_sql(e,
                      "DELETE FROM artifacts WHERE NOT EXISTS(SELECT 1 FROM artifact_refs "
                      "WHERE artifact_id=artifacts.id)",
                      err);
    return rc;
}
ah_code ah_forget(ah_engine *e, const char *sid, bool tree, ah_error *err) {
    ah_code rc = core_guard(e, err);
    if (rc != AH_OK)
        return rc;
    if (!sid)
        return ah_fail(err, AH_INVALID, "session required");
    if (core_jobs_running(e, NULL))
        return ah_fail(err, AH_BUSY, "running tools prevent forgetting");
    pthread_mutex_lock(&e->mailbox_lock);
    bool queued = e->mailbox_head != NULL;
    pthread_mutex_unlock(&e->mailbox_lock);
    if (queued)
        return ah_fail(err, AH_BUSY, "queued steering prevents forgetting");
    core_session session;
    rc = core_load(e, sid, &session, err);
    if (rc != AH_OK)
        return rc;
    core_session_free(&session);
    sqlite3_stmt *s = NULL;
    rc = core_prepare(e, "SELECT root_id FROM agent_nodes WHERE session_id=?", &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
        int step = sqlite3_step(s);
        if (step == SQLITE_ROW && (!tree || strcmp(sid, (const char *)sqlite3_column_text(s, 0))))
            rc = ah_fail(err, AH_BUSY, "forget an agent tree explicitly from its root");
        else if (step != SQLITE_ROW && step != SQLITE_DONE)
            rc = ah_fail(err, AH_IO, "cannot inspect agent tree");
    }
    sqlite3_finalize(s);
    uint64_t after = 0;
    if (rc == AH_OK)
        rc = core_begin(e, &after, err);
    if (rc == AH_OK)
        rc = core_sql(e,
                      "CREATE TEMP TABLE IF NOT EXISTS forget_sessions(id TEXT PRIMARY KEY);"
                      "DELETE FROM forget_sessions",
                      err);
    if (rc == AH_OK)
        rc = core_prepare(e,
                          "INSERT INTO forget_sessions(id) SELECT ?1 UNION "
                          "SELECT session_id FROM agent_nodes WHERE root_id=?1 AND ?2",
                          &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(s, 2, tree);
        rc = core_done(e, s, err);
    }
    if (rc == AH_OK)
        rc = retention_boundary(e, err);
    if (rc == AH_OK)
        rc = forget_rows(e, err);
    if (rc == AH_OK)
        rc = core_sql(e, "DROP TABLE forget_sessions", err);
    if (rc == AH_OK)
        rc = core_commit(e, after, err);
    else if (!sqlite3_get_autocommit(e->db))
        core_rollback(e);
    return rc;
}
ah_code ah_storage_vacuum(ah_engine *e, ah_error *err) {
    ah_code rc = core_guard(e, err);
    if (rc != AH_OK)
        return rc;
    if (core_jobs_running(e, NULL))
        return ah_fail(err, AH_BUSY, "running tools prevent storage vacuum");
    return core_sql(e, "VACUUM", err);
}
