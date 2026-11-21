#include "agents_internal.h"
#include "../core/work.h"
#include "ah/jobs.h"

static ah_code interrupt_operation(ah_engine *e, const char *sid, core_operation *op, bool *blocked,
                                   bool *unknown, ah_error *error) {
    bool draining = false;
    ah_code code = AH_OK;
    if (op->durable) {
        code = core_work_finish_children(e, sid, op->id, error);
        if (code == AH_UNCERTAIN) {
            *unknown = true;
            *blocked = true;
        } else if (code == AH_BUSY)
            draining = true;
        if (code == AH_UNCERTAIN || code == AH_BUSY) {
            code = AH_OK;
            ah_error_clear(error);
        }
    }
    if (code != AH_OK)
        return code;
    bool executing = !strcmp(op->status, "executing");
    if (!strcmp(op->status, "unknown") || (executing && op->effect != AH_EFFECT_READ)) {
        *unknown = true;
        *blocked = true;
        return executing ? core_set_operation(e, sid, op->id, "unknown", error) : AH_OK;
    }
    if (op->durable && executing && ah_cancel_job(e, op->id, NULL) == AH_OK)
        draining = true;
    *blocked |= draining;
    bool received =
        op->result && (!strcmp(op->status, "received") || !strcmp(op->status, "received_late") ||
                       !strcmp(op->status, "resolved"));
    const char *result =
        received ? op->result : "{\"error\":\"agent interrupted before execution\"}";
    const char *event = received ? "tool.completed" : "tool.interrupted";
    bool is_error = received ? op->is_error : true;
    if (!received)
        op->result_format = AH_TOOL_RESULT_JSON;
    /* A received cancellation fences late callback continuations immediately.
     * Publishing waits for owned callbacks to drain and effects to reconcile. */
    if (*blocked)
        return received ? AH_OK
                        : core_save_result(e, sid, op, result, is_error, "received", event, error);
    return core_receipt(e, sid, op, result, is_error, event, error);
}

ah_code agents_stop(ah_engine *e, const char *sender, const char *recipient, ah_error *err) {
    char from[AH_ID_CAP], to[AH_ID_CAP];
    ah_code rc = agents_root(e, sender, from, err);
    if (rc == AH_OK)
        rc = agents_root(e, recipient, to, err);
    if (rc != AH_OK)
        return rc;
    if (strcmp(from, to) || !strcmp(sender, recipient) || !strcmp(to, recipient))
        return ah_fail(err, AH_INVALID, "stop requires another non-root agent in this tree");
    core_jobs_cancel(e, recipient);
    sqlite3_stmt *s = NULL;
    rc = core_prepare(e,
                      "UPDATE agent_nodes SET generation=generation+1,resume_pending=0,"
                      "wake_after=(SELECT COALESCE(MAX(seq),0) FROM agent_messages) "
                      "WHERE session_id=?",
                      &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, recipient, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
    }
    bool unknown = false, blocked = false;
    s = NULL;
    if (rc == AH_OK)
        rc = core_prepare(e,
                          "SELECT id FROM operations WHERE session_id=? AND status!='done' "
                          "AND NOT EXISTS(SELECT 1 FROM work_calls w WHERE "
                          "w.operation_id=operations.id AND w.parent_id IS NOT NULL) "
                          "ORDER BY seq",
                          &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, recipient, -1, SQLITE_TRANSIENT);
        int step;
        while (rc == AH_OK && (step = sqlite3_step(s)) == SQLITE_ROW) {
            core_operation op;
            rc =
                core_get_operation(e, recipient, (const char *)sqlite3_column_text(s, 0), &op, err);
            if (rc != AH_OK)
                break;
            rc = interrupt_operation(e, recipient, &op, &blocked, &unknown, err);
            core_operation_free(&op);
        }
        if (rc == AH_OK && step != SQLITE_DONE)
            rc = ah_fail(err, AH_IO, "cannot settle interrupted agent operations");
    }
    sqlite3_finalize(s);
    s = NULL;
    if (rc == AH_OK)
        rc = core_prepare(e,
                          "DELETE FROM agent_waits WHERE operation_id IN "
                          "(SELECT id FROM operations WHERE session_id=? AND status='done')",
                          &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, recipient, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
    }
    if (rc == AH_OK)
        rc = core_state(e, recipient, unknown ? AH_OUTCOME_UNKNOWN : AH_STOPPED, err);
    char *event = core_object_string("sender", sender);
    if (rc == AH_OK && !event)
        rc = ah_fail(err, AH_IO, "out of memory");
    if (rc == AH_OK)
        rc = core_event(e, recipient, NULL, "agent.interrupted", event, err);
    free(event);
    return rc;
}

ah_code ah_agent_interrupt(ah_engine *e, const char *sender, const char *recipient, ah_error *err) {
    ah_code rc = core_guard(e, err);
    if (rc != AH_OK)
        return rc;
    if (!e->agents_enabled)
        return ah_fail(err, AH_INVALID, "agents are not enabled");
    uint64_t after = 0;
    rc = core_begin(e, &after, err);
    if (rc == AH_OK)
        rc = agents_stop(e, sender, recipient, err);
    if (rc == AH_OK)
        rc = core_commit(e, after, err);
    else
        core_rollback(e);
    return rc;
}
