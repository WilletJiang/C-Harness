#include "core.h"
#include "../tools/local_internal.h"

/* Only classify a fresh, trustworthy failure with a concrete next action.
 * A transport error, denial, timer or uncertain effect is never loop evidence. */
static const char *advice(ah_engine *e, const core_operation *op, yyjson_val *body) {
    yyjson_val *why = yyjson_obj_get(body, "error");
    if (yyjson_equals_str(yyjson_obj_get(body, "kind"), "invalid_arguments") && op->schema) {
        ah_error validation = {0};
        if (ah_schema_validate(op->schema, op->arguments, &validation) == AH_INVALID)
            return "Inspect the tool schema and change the rejected arguments.";
    }
    core_tool *tool = core_find_tool(e, op->name);
    if (!tool)
        return NULL;
    ah_execute_fn fn = tool->tool.execute;
    if ((fn == ah_local_read_file || fn == ah_local_find) &&
        yyjson_equals_str(why, ah_code_name(AH_NOT_FOUND)))
        return "Use find with mode=files from an existing parent directory, then use a returned "
               "path.";
    if (fn == ah_local_find && yyjson_equals_str(why, "stale_snapshot"))
        return "Restart find at offset 0 without the old snapshot; use the new snapshot for later "
               "pages.";
    yyjson_val *conflict = yyjson_obj_get(body, "conflict");
    if (fn == ah_local_apply &&
        yyjson_equals_str(yyjson_obj_get(body, "application"), "not_applied") &&
        yyjson_equals_str(yyjson_obj_get(conflict, "error"), "conflict"))
        return "Read the current file and rebuild the edits against its current revision.";
    return NULL;
}

static bool same_args(const char *a, const char *b) {
    if (!strcmp(a, b))
        return true;
    yyjson_doc *left = yyjson_read(a, strlen(a), 0), *right = yyjson_read(b, strlen(b), 0);
    bool equal =
        left && right && yyjson_equals(yyjson_doc_get_root(left), yyjson_doc_get_root(right));
    yyjson_doc_free(left);
    yyjson_doc_free(right);
    return equal;
}

/* Walk the existing per-session journal backwards, bounded even after many
 * restarts. Prior feedback is eligible only if it was published BEFORE this
 * intent. Identical siblings admitted together are not model retries. */
static bool previous_failure(ah_engine *e, const char *sid, const core_operation *op,
                             core_operation *prior) {
    sqlite3_stmt *s = NULL;
    ah_error ignored = {0};
    if (core_prepare(e,
                     "SELECT v.type,v.operation_id,o.name FROM "
                     "(SELECT seq,type,operation_id FROM events WHERE session_id=? ORDER BY seq "
                     "DESC LIMIT 128) v "
                     "LEFT JOIN operations o ON o.id=v.operation_id ORDER BY v.seq DESC",
                     &s, &ignored) != AH_OK)
        return false;
    sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
    bool before_intent = false, matched = false;
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *type = (const char *)sqlite3_column_text(s, 0);
        const char *id = (const char *)sqlite3_column_text(s, 1);
        if (id && !strcmp(id, op->id)) {
            if (!strcmp(type, "tool.intent") || !strcmp(type, "work.created"))
                before_intent = true;
            continue;
        }
        const char *name = (const char *)sqlite3_column_text(s, 2);
        if (!strcmp(type, "tool.completed") && before_intent && id && name &&
            !strcmp(name, op->name)) {
            matched = core_get_operation(e, sid, id, prior, &ignored) == AH_OK;
            break;
        }
        /* Pure flow orchestration can sit between two real child calls. */
        if (name && !strcmp(name, "flow") &&
            (!strncmp(type, "tool.", 5) || !strncmp(type, "work.", 5)))
            continue;
        if (!strncmp(type, "input.", 6) || !strncmp(type, "tool.", 5) ||
            !strncmp(type, "workspace.", 10) || !strncmp(type, "agent.", 6) ||
            !strncmp(type, "approval.", 9) || !strncmp(type, "work.", 5) ||
            !strncmp(type, "context.", 8))
            break;
    }
    sqlite3_finalize(s);
    return matched;
}

char *core_retry_feedback(ah_engine *e, const char *sid, const core_operation *op,
                          const char *json) {
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    yyjson_val *body = yyjson_doc_get_root(doc);
    const char *hint = advice(e, op, body);
    core_operation prior = {0};
    char *output = NULL;
    if (hint && !yyjson_obj_get(body, "retry_feedback") && previous_failure(e, sid, op, &prior) &&
        prior.is_error && prior.result && prior.schema && op->schema &&
        !strcmp(prior.schema, op->schema) && prior.effect == op->effect &&
        prior.approval == op->approval && same_args(prior.arguments, op->arguments)) {
        yyjson_doc *old = yyjson_read(prior.result, strlen(prior.result), 0);
        yyjson_mut_doc *d = ah_mut_doc_new();
        yyjson_mut_val *last = yyjson_val_mut_copy(d, yyjson_doc_get_root(old)),
                       *current = yyjson_val_mut_copy(d, body);
        yyjson_mut_val *feedback = yyjson_mut_obj_remove_str(last, "retry_feedback");
        uint64_t count = yyjson_mut_get_uint(yyjson_mut_obj_get(feedback, "attempts"));
        if (yyjson_mut_equals(last, current)) {
            yyjson_mut_doc_set_root(d, current);
            yyjson_mut_val *note = yyjson_mut_obj_add_obj(d, current, "retry_feedback");
            yyjson_mut_obj_add_uint(d, note, "attempts",
                                    count >= 2 && count < UINT64_MAX ? count + 1 : 2);
            yyjson_mut_obj_add_str(d, note, "reason", "same_observed_failure");
            yyjson_mut_obj_add_str(d, note, "next_action", hint);
            output = ah_mut_write(d);
            if (output && strlen(output) > e->config.max_output_bytes) {
                free(output);
                output = NULL;
            }
        }
        ah_mut_doc_free(d);
        yyjson_doc_free(old);
    }
    core_operation_free(&prior);
    yyjson_doc_free(doc);
    return output;
}
