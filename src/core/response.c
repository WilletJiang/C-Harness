#include "core.h"
#include "work.h"
#include "../agents/agents_internal.h"

static bool optional_string(yyjson_val *object, const char *key, bool nullable) {
    yyjson_val *value = yyjson_obj_get(object, key);
    return !value || (nullable && yyjson_is_null(value)) || yyjson_is_str(value);
}
static bool content_parts(yyjson_val *parts, const char *text_type) {
    if (!yyjson_is_arr(parts))
        return false;
    size_t index, count;
    yyjson_val *part;
    yyjson_arr_foreach(parts, index, count, part) {
        if (!yyjson_is_obj(part))
            return false;
        const char *type = yyjson_get_str(yyjson_obj_get(part, "type"));
        if (!type)
            return false;
        if (text_type) {
            if (strcmp(type, text_type) || !yyjson_is_str(yyjson_obj_get(part, "text")))
                return false;
        } else if (!strcmp(type, "output_text")) {
            if (!yyjson_is_str(yyjson_obj_get(part, "text")))
                return false;
            yyjson_val *annotations = yyjson_obj_get(part, "annotations"),
                       *logprobs = yyjson_obj_get(part, "logprobs");
            if ((annotations && !yyjson_is_arr(annotations)) ||
                (logprobs && !yyjson_is_arr(logprobs)))
                return false;
        } else if (!strcmp(type, "refusal")) {
            if (!yyjson_is_str(yyjson_obj_get(part, "refusal")))
                return false;
        } else
            return false;
    }
    return true;
}
static ah_code validate_response(ah_engine *e, yyjson_val *root, ah_error *err) {
    const char *status = yyjson_get_str(yyjson_obj_get(root, "status"));
    yyjson_val *out = yyjson_obj_get(root, "output");
    if (!yyjson_is_obj(root) || !status || strcmp(status, "completed") || !yyjson_is_arr(out))
        return ah_fail(err, AH_PROTOCOL,
                       "provider must return a completed Responses object with output array");
    if (!optional_string(root, "id", false) || !optional_string(root, "object", false))
        return ah_fail(err, AH_PROTOCOL, "response identity fields must be strings");
    yyjson_val *failure = yyjson_obj_get(root, "error"),
               *incomplete = yyjson_obj_get(root, "incomplete_details");
    if ((failure && !yyjson_is_null(failure)) || (incomplete && !yyjson_is_null(incomplete)))
        return ah_fail(err, AH_PROTOCOL, "completed response contains failure details");
    size_t index, max;
    yyjson_val *item;
    yyjson_arr_foreach(out, index, max, item) {
        const char *type = yyjson_get_str(yyjson_obj_get(item, "type"));
        if (!yyjson_is_obj(item) || !type)
            return ah_fail(err, AH_PROTOCOL, "output item must be an object with a type");
        if (!optional_string(item, "id", !strcmp(type, "function_call")) ||
            !optional_string(item, "status", strcmp(type, "message") != 0))
            return ah_fail(err, AH_PROTOCOL, "output identity and status must be strings");
        const char *item_status = yyjson_get_str(yyjson_obj_get(item, "status"));
        if (item_status && strcmp(item_status, "completed"))
            return ah_fail(err, AH_PROTOCOL, "output item is not completed");
        if (!strcmp(type, "function_call")) {
            const char *name = yyjson_get_str(yyjson_obj_get(item, "name")),
                       *args = yyjson_get_str(yyjson_obj_get(item, "arguments")),
                       *call = yyjson_get_str(yyjson_obj_get(item, "call_id"));
            const char *space = yyjson_get_str(yyjson_obj_get(item, "namespace"));
            if (!optional_string(item, "namespace", true))
                return ah_fail(err, AH_PROTOCOL, "function namespace must be a string");
            if (!name || !*name || strlen(name) > 64 || !args ||
                strlen(args) > e->config.max_tool_arguments_bytes || !call || !*call ||
                strlen(call) > 256 || (space && strcmp(space, "local")))
                return ah_fail(err, AH_PROTOCOL, "malformed or oversized function call");
            for (size_t j = 0; j < index; j++) {
                yyjson_val *prior = yyjson_arr_get(out, j);
                const char *other = yyjson_get_str(yyjson_obj_get(prior, "call_id"));
                if (other && !strcmp(other, call))
                    return ah_fail(err, AH_PROTOCOL, "duplicate call_id in response");
            }
        } else if (!strcmp(type, "message")) {
            const char *role = yyjson_get_str(yyjson_obj_get(item, "role"));
            if (!role || strcmp(role, "assistant") ||
                !content_parts(yyjson_obj_get(item, "content"), NULL))
                return ah_fail(err, AH_PROTOCOL, "malformed assistant message");
        } else if (!strcmp(type, "reasoning")) {
            yyjson_val *summary = yyjson_obj_get(item, "summary"),
                       *content = yyjson_obj_get(item, "content");
            if (!optional_string(item, "encrypted_content", true) ||
                (summary && !content_parts(summary, "summary_text")) ||
                (content && !yyjson_is_null(content) && !content_parts(content, "reasoning_text")))
                return ah_fail(err, AH_PROTOCOL, "malformed reasoning item");
        } else if (!strcmp(type, "compaction")) {
            yyjson_val *opaque = yyjson_obj_get(item, "encrypted_content");
            if (!e->context_enabled || !e->context_policy.compact_threshold_tokens ||
                !yyjson_is_str(opaque) || !yyjson_get_len(opaque))
                return ah_fail(err, AH_PROTOCOL,
                               "native compaction requires an explicit provider capability and "
                               "encrypted content");
        } else
            return ah_fail(err, AH_PROTOCOL, "unsupported output item type: %s", type);
    }
    return AH_OK;
}
ah_code core_accept_response(ah_engine *e, core_session *session, const char *json, ah_error *err) {
    if (!json || strlen(json) > e->config.max_output_bytes)
        return ah_fail(err, AH_LIMIT, "provider response exceeds output budget");
    yyjson_doc *d = yyjson_read(json, strlen(json), 0);
    if (!d)
        return ah_fail(err, AH_PROTOCOL, "provider returned invalid JSON");
    yyjson_val *root = yyjson_doc_get_root(d);
    ah_error validation = {0};
    ah_code rc = ah_json_check(root, &validation);
    if (rc != AH_OK)
        rc = ah_fail(err, AH_PROTOCOL, "invalid provider JSON: %s", validation.message);
    else
        rc = validate_response(e, root, err);
    uint64_t after = 0;
    if (rc == AH_OK)
        rc = core_begin(e, &after, err);
    sqlite3_int64 prefix_end = 0;
    sqlite3_stmt *prefix = NULL;
    if (rc == AH_OK)
        rc = core_prepare(e, "SELECT COALESCE(MAX(seq),0) FROM items WHERE session_id=?", &prefix,
                          err);
    if (rc == AH_OK) {
        sqlite3_bind_text(prefix, 1, session->id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(prefix) == SQLITE_ROW)
            prefix_end = sqlite3_column_int64(prefix, 0);
        else
            rc = ah_fail(err, AH_IO, "cannot read context boundary");
    }
    sqlite3_finalize(prefix);
    bool native_compaction = false;
    char first_flow[AH_ID_CAP] = {0};
    yyjson_val *output = yyjson_obj_get(root, "output"), *item;
    size_t index, max, calls = 0;
    yyjson_arr_foreach(output, index, max, item) {
        if (rc != AH_OK)
            break;
        char *serialized = ah_json_write(item);
        if (!serialized) {
            rc = ah_fail(err, AH_IO, "out of memory");
            break;
        }
        rc = core_add_item(e, session->id, session->turn, serialized, err);
        free(serialized);
        const char *type = yyjson_get_str(yyjson_obj_get(item, "type"));
        native_compaction |= type && !strcmp(type, "compaction");
        if (rc == AH_OK && type && !strcmp(type, "function_call")) {
            const char *name = yyjson_get_str(yyjson_obj_get(item, "name")),
                       *args = yyjson_get_str(yyjson_obj_get(item, "arguments")),
                       *call = yyjson_get_str(yyjson_obj_get(item, "call_id"));
            core_tool *tool = core_find_tool(e, name);
            char id[AH_ID_CAP];
            rc = core_id(id, err);
            sqlite3_stmt *s = NULL;
            if (rc == AH_OK)
                rc = core_prepare(e, "INSERT INTO call_ids(session_id,call_id) VALUES(?,?)", &s,
                                  err);
            if (rc == AH_OK) {
                sqlite3_bind_text(s, 1, session->id, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 2, call, -1, SQLITE_TRANSIENT);
                rc = core_done(e, s, err);
            }
            if (rc == AH_OK)
                rc = core_prepare(e,
                                  "INSERT INTO "
                                  "operations(id,session_id,call_id,name,arguments,effect,approval,"
                                  "schema,status) VALUES(?,?,?,?,?,?,?,?,'pending')",
                                  &s, err);
            if (rc == AH_OK) {
                sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 2, session->id, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 3, call, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 4, name, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 5, args, -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(s, 6, tool ? tool->tool.effect : AH_EFFECT_READ);
                sqlite3_bind_int(s, 7, tool ? tool->tool.requires_approval : 0);
                const char *schema = tool ? tool->schema : core_builtin_schema(name);
                if (schema)
                    sqlite3_bind_text(s, 8, schema, -1, SQLITE_TRANSIENT);
                rc = core_done(e, s, err);
            }
            if (rc == AH_INVALID)
                rc = ah_fail(err, AH_PROTOCOL, "duplicate call_id across session");
            if (rc == AH_OK && tool && tool->work) {
                core_operation operation = {.id = id};
                rc = core_work_seed(e, session->id, &operation, tool, err);
            }
            if (rc == AH_OK) {
                char *intent = ah_json_write(item);
                if (!intent)
                    rc = ah_fail(err, AH_IO, "out of memory");
                else {
                    rc = core_event(e, session->id, id, "tool.intent", intent, err);
                    free(intent);
                }
            }
            if (rc == AH_OK && !calls && !strcmp(name, "flow"))
                memcpy(first_flow, id, strlen(id) + 1);
            calls++;
        }
    }
    if (rc == AH_OK && native_compaction)
        rc = core_context_native(e, session->id, prefix_end, err);
    if (rc == AH_OK && e->agents_enabled)
        rc = agents_record_usage(e, session, root, err);
    if (rc == AH_OK)
        rc = core_event(e, session->id, NULL, "model.completed", json, err);
    if (rc == AH_OK && calls == 0) {
        pthread_mutex_lock(&e->mailbox_lock);
        bool queued = false;
        for (core_steering *message = e->mailbox_head; message; message = message->next) {
            if (!strcmp(message->session, session->id)) {
                queued = true;
                break;
            }
        }
        if (!queued && !e->agents_enabled && !strcmp(e->active_session, session->id))
            e->active_session[0] = '\0';
        pthread_mutex_unlock(&e->mailbox_lock);
        if (!queued) {
            rc = core_state(e, session->id, AH_COMPLETED, err);
            session->completion_committed = rc == AH_OK;
        }
        session->state = AH_COMPLETED;
    }
    if (rc == AH_OK && first_flow[0] && calls == 1)
        rc = core_flow_stage(e, session, first_flow, err);
    if (rc == AH_OK)
        rc = core_commit(e, after, err);
    else
        core_rollback(e);
    if (rc == AH_OK && native_compaction)
        e->context_history = true;
    yyjson_doc_free(d);
    return rc;
}
