#include "work.h"
#include "flow_vm.h"

typedef struct core_flow_batch {
    unsigned count;
    core_operation ops[16];
} core_flow_batch;

static void free_batch(core_flow_batch *batch) {
    if (!batch)
        return;
    for (unsigned i = 0; i < batch->count; i++)
        core_operation_free(&batch->ops[i]);
    free(batch);
}

void core_flow_clear_staged(core_session *session) {
    free_batch(session->flow_batch);
    session->flow_batch = NULL;
}

/* An opportunistic admission cannot roll back the receipt that made it
 * possible. If it fails, leave the call pending for the ordinary executor. */
static ah_code stage(ah_engine *e, core_session *session, core_flow_batch *batch, ah_error *error) {
    ah_code code = core_sql(e, "SAVEPOINT flow_admission", error);
    if (code == AH_OK) {
        ah_error deferred = {0};
        if (core_record_intents(e, session, batch->ops, batch->count, &deferred) != AH_OK) {
            free_batch(batch);
            return core_sql(e, "ROLLBACK TO flow_admission; RELEASE flow_admission", error);
        }
        code = core_sql(e, "RELEASE flow_admission", error);
    }
    if (code == AH_OK)
        session->flow_batch = batch;
    else
        free_batch(batch);
    return code;
}

/* A flow is already at the head of its session. Admit its first window of
 * ordinary, validated reads in the transaction that saves the checkpoint.
 * Approval, durable callbacks, effects, capacity and insufficient budgets all
 * retain their normal scheduler path. No worker starts until after commit. */
ah_code core_stage_flow_reads(ah_engine *e, core_session *session, const core_operation *parent,
                              ah_error *error) {
    unsigned cap = core_jobs_capacity(e);
    if (session->flow_batch || e->agents_enabled || !cap ||
        session->tool_calls >= e->config.max_tool_calls || core_cancelled(e))
        return AH_OK;
    unsigned remaining = e->config.max_tool_calls - session->tool_calls;
    ah_code rc = core_task_allow(e, session->id, false, 0, &remaining, error);
    if (rc == AH_LIMIT || rc == AH_UNCERTAIN)
        return AH_OK;
    if (rc != AH_OK || !remaining)
        return rc;
    if (cap > remaining)
        cap = remaining;
    core_flow_batch *batch = calloc(1, sizeof(*batch));
    if (!batch)
        return AH_OK;
    rc =
        core_collect_reads(e, session->id, NULL, parent->id, batch->ops, cap, &batch->count, error);
    if (rc == AH_OK && batch->count && core_jobs_init(e, NULL) == AH_OK)
        return stage(e, session, batch, error);
    free_batch(batch);
    return rc;
}

static ah_code stage_head(ah_engine *e, core_session *session, const char *new_id,
                          core_operation *op, ah_error *error) {
    if (!op->durable || op->approval || op->effect != AH_EFFECT_READ || strcmp(op->name, "flow"))
        return AH_OK;
    core_tool *tool = core_find_tool(e, op->name);
    if (!tool || !tool->work || !op->schema || strcmp(op->schema, tool->schema))
        return AH_OK;
    ah_error validation = {0};
    if (new_id) {
        if (strcmp(op->id, new_id) || op->generation || strcmp(op->status, "pending") ||
            ah_schema_validate(tool->schema, op->arguments, &validation) != AH_OK)
            return AH_OK;
    } else {
        const core_flow_cell *cell = e->flow_cells;
        while (cell && strcmp(cell->operation, op->id))
            cell = cell->next;
        if (!cell || cell->next_generation != op->generation + 1 || strcmp(op->status, "parked"))
            return AH_OK;
        bool ready;
        ah_code rc = core_work_ready(e, session->id, op->id, &ready, error);
        if (rc != AH_OK)
            return rc;
        if (!ready)
            return core_stage_flow_reads(e, session, op, error);
    }
    if (core_jobs_running(e, NULL))
        return AH_OK;
    ah_code code = core_task_allow(e, session->id, false, 1, NULL, error);
    if (code != AH_OK)
        return code == AH_LIMIT || code == AH_UNCERTAIN ? AH_OK : code;
    core_flow_batch *batch = calloc(1, sizeof(*batch));
    if (!batch)
        return AH_OK;
    batch->ops[0] = *op;
    memset(op, 0, sizeof(*op));
    batch->count = 1;
    code = stage(e, session, batch, error);
    if (code == AH_OK && new_id && session->flow_batch) {
        /* Initialize the trusted VM while the owner commits admission. */
        core_flow_cell *cell = NULL;
        core_flow_start(e, new_id, &cell, &validation);
    }
    return code;
}

/* The first claim can share the model receipt's transaction. A subsequent
 * claim can share the final child receipt's transaction while its VM is live. */
ah_code core_flow_stage(ah_engine *e, core_session *session, const char *new_id, ah_error *error) {
    if (session->flow_batch || (!new_id && !e->flow_cells) || e->agents_enabled ||
        session->state != AH_RUNNING || !core_jobs_available(e) || core_cancelled(e) ||
        session->tool_calls >= e->config.max_tool_calls)
        return AH_OK;
    core_operation op;
    ah_code code = core_next_operation(e, session->id, &op, error);
    if (code != AH_OK)
        return code == AH_NOT_FOUND ? AH_OK : code;
    code = stage_head(e, session, new_id, &op, error);
    core_operation_free(&op);
    return code;
}

/* The next owner tick consumes committed intents. Detach the batch first so
 * an inline read can stage its parent's next phase without C recursion. */
ah_code core_flow_dispatch_staged(ah_engine *e, core_session *session, ah_error *error) {
    core_flow_batch *batch = session->flow_batch;
    session->flow_batch = NULL;
    session->tool_calls += batch->count;
    core_operation *first = batch->ops;
    ah_code code = batch->count == 1 && !core_jobs_running(e, NULL) &&
                           !core_find_tool(e, first->name)->background
                       ? core_dispatch_tool(e, session, first, false, false, error)
                       : core_dispatch_reads(e, session, first, batch->count, error);
    free_batch(batch);
    return code;
}
