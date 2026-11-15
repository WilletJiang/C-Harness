#include "core.h"

#define MAX_TOOL_WORKERS 16

static bool independent_read(ah_engine *e, const core_operation *op) {
    core_tool *tool = core_find_tool(e, op->name);
    if (op->effect != AH_EFFECT_READ || op->approval || !tool ||
        (!tool->background && !tool->tool.parallel_safe) || tool->tool.effect != op->effect ||
        tool->tool.requires_approval != op->approval || !op->schema ||
        strcmp(tool->schema, op->schema))
        return false;
    ah_error error = {0};
    return ah_schema_validate(tool->schema, op->arguments, &error) == AH_OK;
}

/* Only consecutive independent reads may pass an unfinished earlier read.
 * Everything else, including approval and agent tools, closes the window. */
static ah_code collect_reads(ah_engine *e, const char *sid, core_operation *reads, unsigned limit,
                             unsigned *count, ah_error *error) {
    sqlite3_stmt *stmt = NULL;
    ah_code code = core_prepare(e,
                                "SELECT id FROM operations WHERE session_id=? AND status!='done' "
                                "ORDER BY seq",
                                &stmt, error);
    if (code != AH_OK)
        return code;
    sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_TRANSIENT);
    while (*count < limit) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_DONE)
            break;
        if (step != SQLITE_ROW) {
            code = ah_fail(error, AH_IO, "cannot read pending tool window");
            break;
        }
        core_operation operation;
        code = core_get_operation(e, sid, (const char *)sqlite3_column_text(stmt, 0), &operation,
                                  error);
        if (code != AH_OK)
            break;
        bool eligible = independent_read(e, &operation);
        bool pending = !strcmp(operation.status, "pending");
        bool active = !strcmp(operation.status, "executing") ||
                      !strcmp(operation.status, "received") ||
                      !strcmp(operation.status, "received_late");
        if (eligible && pending)
            reads[(*count)++] = operation;
        else {
            core_operation_free(&operation);
            if (!eligible || !active)
                break;
        }
    }
    sqlite3_finalize(stmt);
    return code;
}

static ah_code launch_reads(ah_engine *e, core_session *session, core_operation *reads,
                            unsigned count, ah_error *error) {
    ah_code code = core_jobs_init(e, error);
    if (code == AH_OK)
        code = core_tool_intents(e, session, reads, count, error);
    if (code != AH_OK)
        return code;
    for (unsigned i = 0; i < count; i++) {
        if (code == AH_OK) {
            code = core_dispatch_tool(e, session, &reads[i], true, false, error);
            if (code != AH_OK)
                core_jobs_cancel(e, session->id);
        } else {
            /* Every committed intent gets a receipt even if an earlier launch
             * failed. No undispatched callback can have produced an effect. */
            ah_tool_result result = {
                .json = core_object_string("error", "tool worker launch failed"), .is_error = true};
            ah_error failure = {0};
            ah_code captured = core_capture_operation(e, session, &reads[i], &result, AH_OK,
                                                      &failure, AH_OK, &failure);
            free(result.json);
            if (captured != AH_OK) {
                code = captured;
                if (error)
                    *error = failure;
            }
        }
    }
    if (code != AH_OK) {
        ah_error failure = {0};
        ah_code published = core_publish_ready(e, session, &failure);
        if (published != AH_OK) {
            code = published;
            if (error)
                *error = failure;
        }
    }
    return code;
}

ah_code core_tools_step(ah_engine *e, core_session *session, bool *pending, ah_error *error) {
    *pending = true;
    ah_code code = core_jobs_running(e, session->id) ? core_jobs_finish(e, session, error) : AH_OK;
    if (code != AH_OK)
        return code;
    core_operation first;
    code = core_next_operation(e, session->id, &first, error);
    if (code == AH_NOT_FOUND) {
        *pending = false;
        return AH_OK;
    }
    if (code != AH_OK)
        return code;
    if (!strcmp(first.status, "received") || !strcmp(first.status, "received_late") ||
        !strcmp(first.status, "resolved") || !strcmp(first.status, "unknown")) {
        core_operation_free(&first);
        code = core_publish_ready(e, session, error);
        if (code != AH_OK)
            return code;
        code = core_next_operation(e, session->id, &first, error);
        if (code == AH_NOT_FOUND) {
            *pending = false;
            return AH_OK;
        }
        if (code != AH_OK)
            return code;
    }
    bool running = core_jobs_running(e, session->id);
    if (independent_read(e, &first) &&
        (!strcmp(first.status, "pending") || !strcmp(first.status, "executing"))) {
        unsigned limit = core_jobs_capacity(e);
        unsigned remaining = session->tool_calls < e->config.max_tool_calls
                                 ? e->config.max_tool_calls - session->tool_calls
                                 : 0;
        if (limit > remaining)
            limit = remaining;
        if (limit) {
            code = core_task_allow(e, session->id, false, 0, &remaining, error);
            if (limit > remaining)
                limit = remaining;
        }
        core_operation reads[MAX_TOOL_WORKERS] = {0};
        unsigned count = 0;
        if (code == AH_OK && limit)
            code = collect_reads(e, session->id, reads, limit, &count, error);
        bool workers = count && (count > 1 || running || core_jobs_running(e, NULL) ||
                                 core_find_tool(e, reads[0].name)->background);
        if (code == AH_OK && workers)
            code = launch_reads(e, session, reads, count, error);
        else if (code == AH_OK && !running && !core_jobs_running(e, NULL))
            code = core_execute_operation(e, session, &first, error);
        for (unsigned i = 0; i < count; i++)
            core_operation_free(&reads[i]);
    } else if (!running)
        code = core_execute_operation(e, session, &first, error);
    core_operation_free(&first);
    if (code == AH_OK && !core_jobs_running(e, session->id)) {
        code = core_next_operation(e, session->id, &first, error);
        *pending = code != AH_NOT_FOUND;
        if (code == AH_OK)
            core_operation_free(&first);
        else if (code == AH_NOT_FOUND)
            code = AH_OK;
    }
    return code;
}
