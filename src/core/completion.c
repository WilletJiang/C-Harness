#include "core.h"
#include "work.h"

static ah_code complete(ah_engine *e, core_session *session, core_operation *op,
                        ah_tool_result *result, ah_code executed, const ah_error *execution,
                        ah_code stopped, bool publish, ah_error *error) {
    bool valid = result->json && strlen(result->json) <= e->config.max_output_bytes &&
                 ah_json_valid(result->json);
    bool owned = sqlite3_get_autocommit(e->db) != 0;
    uint64_t after = 0;
    ah_code code = owned ? core_begin(e, &after, error) : AH_OK;
    ah_error normalization = {0};
    if (code == AH_OK && stopped == AH_OK && result->continuation_json && !result->json &&
        executed == AH_OK && !result->is_error && !result->outcome_unknown) {
        code = core_sql(e, "SAVEPOINT continuation", error);
        if (code == AH_OK)
            code = core_work_apply(e, session->id, op->id, op->generation,
                                   result->continuation_json, &normalization);
        if (code == AH_OK)
            code = core_sql(e, "RELEASE continuation", error);
        if (code == AH_OK) {
            if (owned)
                code = core_commit(e, after, error);
            if (code == AH_OK && stopped != AH_OK)
                code = ah_fail(error, stopped, "durable checkpoint saved after interruption");
            return code;
        }
        ah_code rollback = core_sql(e, "ROLLBACK TO continuation; RELEASE continuation", error);
        if (rollback != AH_OK || (code != AH_INVALID && code != AH_LIMIT && code != AH_NOT_FOUND)) {
            if (owned)
                core_rollback(e);
            session->state = AH_OUTCOME_UNKNOWN;
            return rollback == AH_OK ? code : rollback;
        }
        execution = &normalization;
        code = AH_OK;
    }
    if (result->continuation_json)
        valid = false;
    if (op->hidden)
        publish = true;
    if (code == AH_OK && valid && !result->outcome_unknown &&
        (op->effect == AH_EFFECT_READ || executed == AH_OK)) {
        bool savepoint = !owned && result->format == AH_TOOL_RESULT_CONTENT;
        if (savepoint)
            code = core_sql(e, "SAVEPOINT normalize_result", error);
        if (code == AH_OK && result->format == AH_TOOL_RESULT_JSON)
            op->result_format = AH_TOOL_RESULT_JSON;
        else if (code == AH_OK)
            code = core_normalize_result(e, session->id, op, result, &normalization);
        if (code != AH_OK && (code == AH_INVALID || code == AH_LIMIT || code == AH_NOT_FOUND)) {
            valid = false;
            execution = &normalization;
            if (owned) {
                core_rollback(e);
                code = core_begin(e, &after, error);
            } else
                code = savepoint ? core_sql(e, "ROLLBACK TO normalize_result", error) : AH_OK;
        } else if (code != AH_OK && error && normalization.code != AH_OK)
            *error = normalization;
        if (code == AH_OK && savepoint)
            code = core_sql(e, "RELEASE normalize_result", error);
    }
    bool unknown =
        result->outcome_unknown || (op->effect != AH_EFFECT_READ && (executed != AH_OK || !valid));
    if (code == AH_OK && !unknown && op->durable && publish) {
        ah_code children = core_work_finish_children(e, session->id, op->id, error);
        if (children == AH_BUSY || children == AH_UNCERTAIN) {
            publish = false;
            if (children == AH_UNCERTAIN) {
                code = core_state(e, session->id, AH_OUTCOME_UNKNOWN, error);
                session->state = AH_OUTCOME_UNKNOWN;
            }
        } else if (children != AH_OK)
            code = children;
    }
    if (code == AH_OK && !valid && !unknown) {
        free(result->json);
        result->json = core_object_string(
            "error", execution->message[0] ? execution->message
                                           : "tool returned no valid bounded JSON result");
        result->is_error = true;
        result->format = AH_TOOL_RESULT_JSON;
        op->result_format = AH_TOOL_RESULT_JSON;
        if (!result->json)
            code = ah_fail(error, AH_IO, "out of memory recording tool failure");
    }
    if (code == AH_OK && unknown) {
        code = core_set_operation(e, session->id, op->id, "unknown", error);
        if (code == AH_OK)
            code = core_event(
                e, session->id, op->id, "tool.outcome_unknown",
                valid && result->format == AH_TOOL_RESULT_JSON
                    ? result->json
                    : "{\"reason\":\"tool failed without a trustworthy completion receipt\"}",
                error);
        if (code == AH_OK && publish)
            code = core_state(e, session->id, AH_OUTCOME_UNKNOWN, error);
    } else if (code == AH_OK && publish)
        code = core_receipt(e, session->id, op, result->json, result->is_error || executed != AH_OK,
                            stopped != AH_OK ? "tool.late_result" : "tool.completed", error);
    else if (code == AH_OK)
        code = core_save_result(e, session->id, op, result->json,
                                result->is_error || executed != AH_OK,
                                stopped != AH_OK ? "received_late" : "received",
                                stopped != AH_OK ? "tool.received_late" : "tool.received", error);
    if (owned) {
        if (code == AH_OK)
            code = core_commit(e, after, error);
        else
            core_rollback(e);
    }
    if (code != AH_OK) {
        session->state = AH_OUTCOME_UNKNOWN;
        return code;
    }
    if (!publish)
        return AH_OK;
    if (unknown) {
        session->state = AH_OUTCOME_UNKNOWN;
        return ah_fail(error, AH_UNCERTAIN,
                       "tool outcome is unknown; explicit resolution required");
    }
    if (stopped != AH_OK)
        return ah_fail(error, stopped,
                       "tool completed after cancellation or deadline; receipt recorded");
    return AH_OK;
}

ah_code core_complete_operation(ah_engine *e, core_session *session, core_operation *op,
                                ah_tool_result *result, ah_code executed, const ah_error *execution,
                                ah_code stopped, ah_error *error) {
    return complete(e, session, op, result, executed, execution, stopped, true, error);
}

ah_code core_capture_operation(ah_engine *e, core_session *session, core_operation *op,
                               ah_tool_result *result, ah_code executed, const ah_error *execution,
                               ah_code stopped, ah_error *error) {
    return complete(e, session, op, result, executed, execution, stopped, false, error);
}
