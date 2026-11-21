#include "core.h"
#include "work.h"
#include "../agents/agents_internal.h"

static ah_code count_calls(ah_engine *, const char *, unsigned, ah_error *);

typedef struct {
    ah_engine *engine;
    const char *session, *operation;
    unsigned count;
} checkpoint_context;

static ah_code checkpoint(void *context, const char *json, ah_error *error) {
    checkpoint_context *c = context;
    if (c->engine->observing || !json || strlen(json) > 16384 || !ah_json_valid(json))
        return ah_fail(error, AH_INVALID, "checkpoint must be bounded JSON on the owner callback");
    if (c->count >= 128)
        return ah_fail(error, AH_LIMIT, "checkpoint count exceeded");
    uint64_t after = 0;
    ah_code code = core_begin(c->engine, &after, error);
    if (code == AH_OK)
        code = core_event(c->engine, c->session, c->operation, "tool.checkpoint", json, error);
    if (code == AH_OK)
        code = core_commit(c->engine, after, error);
    else
        core_rollback(c->engine);
    if (code == AH_OK)
        c->count++;
    return code;
}

ah_code core_job_checkpoint(ah_engine *e, const char *session, const char *operation,
                            const char *json, ah_error *error) {
    checkpoint_context context = {e, session, operation, 0};
    return checkpoint(&context, json, error);
}

typedef enum { TOOL_REJECTED, ARGUMENTS_REJECTED, INTERRUPTED_BEFORE_DISPATCH } rejection;

static ah_code finish_error(ah_engine *e, core_session *session, core_operation *op,
                            const char *message, rejection reason, ah_error *err) {
    bool count = reason != INTERRUPTED_BEFORE_DISPATCH;
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *body = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, body);
    yyjson_mut_obj_add_str(doc, body, "error", message);
    if (reason == ARGUMENTS_REJECTED)
        yyjson_mut_obj_add_str(doc, body, "kind", "invalid_arguments");
    char *json = ah_mut_write(doc);
    ah_mut_doc_free(doc);
    if (!json)
        return ah_fail(err, AH_IO, "out of memory");
    uint64_t after = 0;
    ah_code rc = core_begin(e, &after, err);
    if (rc == AH_OK && count)
        rc = count_calls(e, session->id, 1, err);
    if (rc == AH_OK)
        rc = core_receipt(e, session->id, op, json, true, "tool.completed", err);
    if (rc == AH_OK) {
        rc = core_commit(e, after, err);
        if (rc != AH_OK)
            session->state = AH_OUTCOME_UNKNOWN;
    } else
        core_rollback(e);
    if (rc == AH_OK && count)
        session->tool_calls++;
    free(json);
    return rc;
}
static ah_code set_pause(ah_engine *e, core_session *session, core_operation *op,
                         const char *status, ah_state state, const char *event, ah_error *err) {
    uint64_t after = 0;
    ah_code rc = core_begin(e, &after, err);
    if (rc == AH_OK)
        rc = core_set_operation(e, session->id, op->id, status, err);
    if (rc == AH_OK)
        rc = core_state(e, session->id, state, err);
    if (rc == AH_OK)
        rc = core_event(e, session->id, op->id, event, op->arguments, err);
    if (rc == AH_OK) {
        rc = core_commit(e, after, err);
        if (rc != AH_OK)
            session->state = AH_OUTCOME_UNKNOWN;
    } else
        core_rollback(e);
    if (rc == AH_OK)
        session->state = state;
    return rc;
}
ah_code core_pause_approval(ah_engine *e, core_session *session, core_operation *op,
                            ah_error *error) {
    return set_pause(e, session, op, "approval", AH_WAITING_APPROVAL, "approval.requested", error);
}
static ah_code count_calls(ah_engine *e, const char *sid, unsigned count, ah_error *err) {
    sqlite3_stmt *s = NULL;
    ah_code rc =
        core_prepare(e, "UPDATE sessions SET tool_calls=tool_calls+?2 WHERE id=?1", &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, count);
    rc = core_done(e, s, err);
    return rc == AH_OK ? core_task_charge(e, sid, false, count, err) : rc;
}
static ah_code publish_received(ah_engine *e, core_session *session, core_operation *op,
                                ah_error *err) {
    if (!op->result)
        return ah_fail(err, AH_IO, "stored tool receipt unavailable");
    bool owned = sqlite3_get_autocommit(e->db) != 0;
    uint64_t after = 0;
    ah_code rc = owned ? core_begin(e, &after, err) : AH_OK;
    if (rc == AH_OK && op->durable)
        rc = core_work_finish_children(e, session->id, op->id, err);
    if (rc == AH_BUSY || rc == AH_UNCERTAIN) {
        ah_code waiting = rc;
        if (waiting == AH_UNCERTAIN) {
            rc = core_state(e, session->id, AH_OUTCOME_UNKNOWN, err);
            session->state = AH_OUTCOME_UNKNOWN;
        } else
            rc = AH_OK;
        if (owned) {
            if (rc == AH_OK)
                rc = core_commit(e, after, err);
            else
                core_rollback(e);
        }
        return rc == AH_OK ? waiting : rc;
    }
    if (rc == AH_OK)
        rc = core_receipt(
            e, session->id, op, op->result, op->is_error,
            !strcmp(op->status, "received_late") ? "tool.late_result" : "tool.completed", err);
    if (owned) {
        if (rc == AH_OK)
            rc = core_commit(e, after, err);
        else
            core_rollback(e);
    }
    if (rc != AH_OK)
        session->state = AH_OUTCOME_UNKNOWN;
    return rc;
}
ah_code core_publish_ready(ah_engine *e, core_session *session, ah_error *err) {
    for (;;) {
        core_operation op;
        ah_code rc = core_next_operation(e, session->id, &op, err);
        if (rc == AH_NOT_FOUND)
            return AH_OK;
        if (rc != AH_OK)
            return rc;
        bool received = !strcmp(op.status, "received") || !strcmp(op.status, "received_late") ||
                        !strcmp(op.status, "resolved");
        bool unknown = !strcmp(op.status, "unknown");
        if (received)
            rc = publish_received(e, session, &op, err);
        else if (unknown) {
            bool owned = sqlite3_get_autocommit(e->db) != 0;
            uint64_t after = 0;
            rc = owned ? core_begin(e, &after, err) : AH_OK;
            if (rc == AH_OK)
                rc = core_state(e, session->id, AH_OUTCOME_UNKNOWN, err);
            if (owned) {
                if (rc == AH_OK)
                    rc = core_commit(e, after, err);
                else
                    core_rollback(e);
            }
            session->state = AH_OUTCOME_UNKNOWN;
            if (rc == AH_OK)
                rc = ah_fail(err, AH_UNCERTAIN, "resolve uncertain tool outcome before continuing");
        }
        core_operation_free(&op);
        if (rc == AH_BUSY)
            return AH_OK;
        if (rc != AH_OK || !received)
            return rc;
    }
}

ah_code core_execute_operation(ah_engine *e, core_session *session, core_operation *op,
                               ah_error *err) {
    if (!strcmp(op->status, "approval")) {
        session->state = AH_WAITING_APPROVAL;
        return AH_OK;
    }
    if (!strcmp(op->status, "input")) {
        session->state = AH_WAITING_INPUT;
        return AH_OK;
    }
    if (!strcmp(op->status, "unknown")) {
        session->state = AH_OUTCOME_UNKNOWN;
        return ah_fail(err, AH_UNCERTAIN, "resolve uncertain tool outcome before continuing");
    }
    if (!strcmp(op->status, "received") || !strcmp(op->status, "received_late") ||
        !strcmp(op->status, "resolved")) {
        ah_code published = publish_received(e, session, op, err);
        return published == AH_BUSY ? AH_OK : published;
    }
    if (e->agents_enabled && agents_tool_schema(op->name))
        return agents_execute(e, session, op, err);
    if (strcmp(op->status, "pending") && strcmp(op->status, "approved") &&
        strcmp(op->status, "parked") && strcmp(op->status, "work_ready"))
        return ah_fail(err, AH_PROTOCOL, "unexpected operation status");
    if (session->tool_calls >= e->config.max_tool_calls)
        return ah_fail(err, AH_LIMIT, "tool call budget exhausted");
    ah_code allowed = core_task_allow(e, session->id, false, 1, NULL, err);
    if (allowed != AH_OK)
        return allowed;
    core_tool *registered = core_find_tool(e, op->name);
    bool paused = false;
    ah_code prepared = op->durable || (registered && registered->work)
                           ? core_work_prepare(e, session, op, registered, &paused, err)
                           : AH_OK;
    if (prepared != AH_OK || paused)
        return prepared;
    const char *builtin = agents_tool_schema(op->name) ? NULL : core_builtin_schema(op->name),
               *schema = builtin ? builtin : (registered ? registered->schema : NULL);
    if (!schema)
        return finish_error(e, session, op, "tool is unavailable", TOOL_REJECTED, err);
    if (registered && (registered->tool.effect != op->effect ||
                       registered->tool.requires_approval != op->approval || !op->schema ||
                       strcmp(registered->schema, op->schema)))
        return finish_error(e, session, op, "tool policy changed since intent; execution refused",
                            TOOL_REJECTED, err);
    ah_error validation = {0};
    ah_code valid = ah_schema_validate(schema, op->arguments, &validation);
    if (valid != AH_OK)
        return finish_error(e, session, op,
                            validation.message[0] ? validation.message
                                                  : "tool arguments failed schema validation",
                            ARGUMENTS_REJECTED, err);
    if (op->approval && op->decision != 1)
        return core_pause_approval(e, session, op, err);
    if (builtin && strcmp(op->name, "read_result") && strcmp(op->name, "recall")) {
        uint64_t after = 0;
        ah_code rc = core_begin(e, &after, err);
        if (rc == AH_OK)
            rc = count_calls(e, session->id, 1, err);
        if (rc == AH_OK)
            rc = core_set_operation(e, session->id, op->id, "input", err);
        if (rc == AH_OK)
            rc = core_state(e, session->id, AH_WAITING_INPUT, err);
        if (rc == AH_OK)
            rc = core_event(e, session->id, op->id, "input.requested", op->arguments, err);
        if (rc == AH_OK) {
            rc = core_commit(e, after, err);
            if (rc != AH_OK)
                session->state = AH_OUTCOME_UNKNOWN;
        } else
            core_rollback(e);
        if (rc == AH_OK) {
            session->tool_calls++;
            session->state = AH_WAITING_INPUT;
        }
        return rc;
    }
    if (!core_jobs_available(e))
        return AH_OK;
    bool coordinated = false;
    ah_code authorized =
        core_workspace_authorize(e, session->id, op, registered, &coordinated, err);
    if (authorized == AH_BUSY)
        return AH_OK;
    if (authorized == AH_AUTH || authorized == AH_INVALID)
        return finish_error(e, session, op, err ? err->message : "workspace access denied",
                            TOOL_REJECTED, err);
    if (authorized != AH_OK)
        return authorized;
    ah_code rc = core_tool_intents(e, session, op, 1, err);
    if (rc != AH_OK)
        return rc;
    return core_dispatch_tool(e, session, op, registered && registered->background, coordinated,
                              err);
}

ah_code core_record_intents(ah_engine *e, core_session *session, core_operation *operations,
                            unsigned count, ah_error *err) {
    ah_code rc = count_calls(e, session->id, count, err);
    for (unsigned i = 0; i < count && rc == AH_OK; i++) {
        core_operation *op = &operations[i];
        core_tool *tool = core_find_tool(e, op->name);
        if (tool && tool->work)
            rc = core_work_claim(e, session->id, op, err);
        else
            rc = core_set_operation(e, session->id, op->id, "executing", err);
        if (rc == AH_OK)
            rc = core_event(e, session->id, op->id, "tool.started", op->arguments, err);
    }
    if (rc == AH_OK && session->state == AH_WAITING_WORK)
        rc = core_state(e, session->id, AH_RUNNING, err);
    return rc;
}
ah_code core_tool_intents(ah_engine *e, core_session *session, core_operation *operations,
                          unsigned count, ah_error *err) {
    uint64_t after = 0;
    ah_code rc = core_begin(e, &after, err);
    if (rc == AH_OK)
        rc = core_record_intents(e, session, operations, count, err);
    if (rc == AH_OK) {
        rc = core_commit(e, after, err);
        if (rc != AH_OK)
            session->state = AH_OUTCOME_UNKNOWN;
    } else
        core_rollback(e);
    if (rc == AH_OK) {
        session->tool_calls += count;
        if (session->state == AH_WAITING_WORK)
            session->state = AH_RUNNING;
    }
    return rc;
}

ah_code core_dispatch_tool(ah_engine *e, core_session *session, core_operation *op, bool worker,
                           bool coordinated, ah_error *err) {
    core_tool *registered = core_find_tool(e, op->name);
    const char *builtin = core_builtin_schema(op->name);
    ah_code rc;
    core_work work = {0};
    if (registered && registered->work) {
        rc = core_work_load(e, session->id, op->id, &work, err);
        if (rc != AH_OK)
            return rc;
    }

    if (!worker && core_cancelled(e)) {
        core_work_free(&work);
        rc = finish_error(e, session, op, "cancelled before execution", INTERRUPTED_BEFORE_DISPATCH,
                          err);
        if (rc != AH_OK)
            session->state = AH_OUTCOME_UNKNOWN;
        return rc == AH_OK ? ah_fail(err, AH_CANCELLED, "cancelled before tool execution") : rc;
    }
    uint64_t run_deadline = e->deadline, now = ah_now_ms(),
             tool_deadline = now + e->config.tool_timeout_ms;
    if (tool_deadline < now)
        tool_deadline = UINT64_MAX;
    if (run_deadline && tool_deadline > run_deadline)
        tool_deadline = run_deadline;
    e->deadline = tool_deadline;
    checkpoint_context checkpoints = {e, session->id, op->id, 0};
    ah_tool_call call = {.session_id = session->id,
                         .operation_id = op->id,
                         .arguments_json = op->arguments,
                         .cancelled = core_cancelled,
                         .cancel_context = e,
                         .deadline_ms = tool_deadline,
                         .output_limit = e->config.max_output_bytes,
                         .model_output_limit = e->config.max_model_tool_bytes,
                         .checkpoint = op->effect == AH_EFFECT_READ ? NULL : checkpoint,
                         .checkpoint_context = &checkpoints,
                         .yield = e->yield,
                         .yield_context = e->yield_context,
                         .require_revision = coordinated,
                         .checkpoint_json =
                             registered && registered->work
                                 ? (work.checkpoint_json ? work.checkpoint_json : "{}")
                                 : NULL,
                         .wake_json = work.wake_json,
                         .generation = op->generation};
    if (worker) {
        e->deadline = run_deadline;
        rc = core_jobs_start(e, session, op, registered, &call, err);
        core_work_free(&work);
        return rc;
    }
    ah_tool_result result = {0};
    ah_error execution = {0};
    uint64_t started = ah_now_us();
    e->inline_tools++;
    ah_code executed =
        builtin ? (!strcmp(op->name, "recall") ? core_context_recall(e, &call, &result, &execution)
                                               : core_read_result(e, &call, &result, &execution))
                : registered->tool.execute(registered->tool.context, &call, &result, &execution);
    e->inline_tools--;
    core_metric(e, session->id, op->id, "tool", started, ah_now_us(),
                result.json ? strlen(result.json) : 0, 1, executed);
    ah_code stopped = !core_cancelled(e)                                          ? AH_OK
                      : atomic_load_explicit(&e->cancelled, memory_order_relaxed) ? AH_CANCELLED
                                                                                  : AH_LIMIT;
    e->deadline = run_deadline;
    rc = core_complete_operation(e, session, op, &result, executed, &execution, stopped, err);
    free(result.json);
    free(result.continuation_json);
    core_work_free(&work);
    return rc;
}
