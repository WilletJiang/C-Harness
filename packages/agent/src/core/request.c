#include "core.h"
#include "../extensions/extensions_internal.h"

static bool uses_developer_prefix(const char *model) {
    const char family[] = "gpt-6.2";
    size_t n = sizeof(family) - 1;
    return !strncmp(model, family, n) && (model[n] == '\0' || model[n] == '-');
}

static ah_code build_request(ah_engine *e, const core_session *session, char **json,
                             ah_error *err) {
    *json = NULL;
    sqlite3_stmt *measure = NULL;
    ah_code measured = core_prepare(
        e,
        "SELECT COALESCE(SUM(length(i.json)),0) FROM items i WHERE i.session_id=? AND i.active=1 "
        "AND (?2 OR NOT EXISTS(SELECT 1 FROM extension_context x WHERE x.item_seq=i.seq))",
        &measure, err);
    if (measured != AH_OK)
        return measured;
    sqlite3_bind_text(measure, 1, session->id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(measure, 2, e->extensions && e->extensions->config.enable_skills);
    int measured_step = sqlite3_step(measure);
    sqlite3_int64 bytes = measured_step == SQLITE_ROW ? sqlite3_column_int64(measure, 0) : -1;
    sqlite3_finalize(measure);
    if (bytes < 0)
        return ah_fail(err, AH_IO, "cannot measure model context");
    if ((uint64_t)bytes > e->config.max_context_bytes)
        return ah_fail(err, AH_LIMIT,
                       "stored context exceeds byte budget; compact after all tools are settled");
    yyjson_mut_doc *d = ah_mut_doc_new();
    if (!d)
        return ah_fail(err, AH_IO, "out of memory");
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, root);
    yyjson_mut_obj_add_strcpy(d, root, "model", session->model);
    yyjson_mut_obj_add_strcpy(d, root, "prompt_cache_key", session->id);
    bool developer_prefix = uses_developer_prefix(session->model);
    if (!developer_prefix)
        yyjson_mut_obj_add_strcpy(d, root, "instructions", session->instructions);
    yyjson_mut_obj_add_bool(d, root, "store", false);
    yyjson_mut_obj_add_bool(d, root, "stream", true);
    if (session->reasoning[0]) {
        yyjson_mut_val *reasoning = yyjson_mut_obj(d);
        if (!yyjson_mut_obj_add_strcpy(d, reasoning, "effort", session->reasoning) ||
            !yyjson_mut_obj_add_val(d, root, "reasoning", reasoning)) {
            ah_mut_doc_free(d);
            return ah_fail(err, AH_IO, "cannot encode reasoning effort");
        }
    }
    if (e->context_enabled && e->context_policy.compact_threshold_tokens) {
        yyjson_mut_val *management = yyjson_mut_arr(d), *policy = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, policy, "type", "compaction");
        yyjson_mut_obj_add_uint(d, policy, "compact_threshold",
                                e->context_policy.compact_threshold_tokens);
        yyjson_mut_arr_append(management, policy);
        yyjson_mut_obj_add_val(d, root, "context_management", management);
    }
    yyjson_mut_val *include = yyjson_mut_arr(d);
    yyjson_mut_arr_add_str(d, include, "reasoning.encrypted_content");
    yyjson_mut_obj_add_val(d, root, "include", include);
    yyjson_mut_val *input = yyjson_mut_arr(d);
    yyjson_mut_obj_add_val(d, root, "input", input);
    /* GPT-6.2 renders top-level instructions after all cacheable input. */
    if (developer_prefix && *session->instructions) {
        yyjson_mut_val *message = yyjson_mut_arr_add_obj(d, input);
        yyjson_mut_obj_add_str(d, message, "role", "developer");
        yyjson_mut_obj_add_strcpy(d, message, "content", session->instructions);
    }
    if (*session->summary) {
        yyjson_mut_val *item = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, item, "role", "user");
        yyjson_mut_obj_add_strcpy(d, item, "content", session->summary);
        yyjson_mut_arr_append(input, item);
    }
    sqlite3_stmt *s = NULL;
    ah_code rc = AH_OK;
    bool recall = e->context_enabled;
    yyjson_mut_val *note = NULL;
    sqlite3_int64 boundary_seq = 0, boundary_turn = 0, boundary_round = 0;
    if (e->context_enabled || e->context_history) {
        rc = core_prepare(
            e,
            "SELECT c.summary,g.seq,g.turn,g.round "
            "FROM context_checkpoint_refs r JOIN context_checkpoints c ON c.id=r.checkpoint_id "
            "LEFT JOIN items g ON g.seq=COALESCE("
            "(SELECT MIN(seq) FROM items WHERE session_id=?1 AND active=0),"
            "(SELECT MIN(seq) FROM items WHERE session_id=?1 AND "
            "json_extract(json,'$.type')='compaction')) "
            "WHERE r.session_id=?1 ORDER BY c.rowid DESC LIMIT 1",
            &s, err);
        if (rc == AH_OK) {
            sqlite3_bind_text(s, 1, session->id, -1, SQLITE_TRANSIENT);
            int step = sqlite3_step(s);
            if (step == SQLITE_ROW) {
                recall = true;
                boundary_seq = sqlite3_column_int64(s, 1);
                boundary_turn = sqlite3_column_int64(s, 2);
                boundary_round = sqlite3_column_int64(s, 3);
                note = yyjson_mut_obj(d);
                yyjson_mut_obj_add_str(d, note, "role", "user");
                yyjson_mut_obj_add_strcpy(d, note, "content",
                                          (const char *)sqlite3_column_text(s, 0));
            } else if (step != SQLITE_DONE)
                rc = ah_fail(err, AH_IO, "cannot read context checkpoint");
        }
        sqlite3_finalize(s);
        s = NULL;
    }
    if (rc == AH_OK)
        rc = core_prepare(
            e,
            "SELECT i.json,i.seq,i.turn,i.round FROM items i WHERE i.session_id=? AND i.active=1 "
            "AND (?2 OR NOT EXISTS "
            "(SELECT 1 FROM extension_context x WHERE x.item_seq=i.seq)) ORDER BY i.seq",
            &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, session->id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(s, 2, e->extensions && e->extensions->config.enable_skills);
        int step;
        size_t expanded_bytes = 0;
        while ((step = sqlite3_step(s)) == SQLITE_ROW) {
            const char *text = (const char *)sqlite3_column_text(s, 0);
            yyjson_doc *item = yyjson_read(text, strlen(text), 0);
            if (!item) {
                rc = ah_fail(err, AH_IO, "corrupt stored context");
                break;
            }
            yyjson_val *value = yyjson_doc_get_root(item);
            if (note) {
                yyjson_val *role = yyjson_obj_get(value, "role");
                /* Place the note at the first gap. If it cuts a model response, put
                 * the note before that exchange's reasoning, calls and receipts. */
                bool in_exchange =
                    sqlite3_column_int64(s, 2) == boundary_turn &&
                    sqlite3_column_int64(s, 3) == boundary_round &&
                    !(yyjson_equals_str(role, "user") || yyjson_equals_str(role, "developer") ||
                      yyjson_equals_str(role, "system"));
                if (sqlite3_column_int64(s, 1) >= boundary_seq || in_exchange) {
                    yyjson_mut_arr_append(input, note);
                    note = NULL;
                }
            }
            yyjson_mut_val *copy = yyjson_val_mut_copy(d, value);
            rc = core_content_expand(e, session->id, d, copy, &expanded_bytes, err);
            if (rc == AH_OK)
                yyjson_mut_arr_append(input, copy);
            yyjson_doc_free(item);
            if (rc != AH_OK)
                break;
        }
        if (rc == AH_OK && step != SQLITE_DONE)
            rc = ah_fail(err, AH_IO, "context read failed");
    }
    sqlite3_finalize(s);
    if (note)
        yyjson_mut_arr_append(input, note);
    if (rc == AH_OK)
        rc = core_request_tools(e, d, root, recall, err);
    if (rc == AH_OK) {
        *json = ah_mut_write(d);
        if (!*json)
            rc = ah_fail(err, AH_IO, "out of memory");
        else if (strlen(*json) > e->config.max_context_bytes) {
            free(*json);
            *json = NULL;
            rc = ah_fail(
                err, AH_LIMIT,
                "model request exceeds context byte budget; compact after all tools are settled");
        }
    }
    ah_mut_doc_free(d);
    return rc;
}

ah_code core_request(ah_engine *e, const core_session *session, char **json, ah_error *err) {
    *json = NULL;
    ah_code rc = ext_context_sync(e, session->id, err);
    if (rc != AH_OK)
        return rc;
    rc = build_request(e, session, json, err);
    if (!e->context_enabled || (rc != AH_OK && rc != AH_LIMIT))
        return rc;
    size_t bytes = *json ? strlen(*json) : 0;
    if (rc == AH_LIMIT) {
        sqlite3_stmt *s = NULL;
        ah_code measured =
            core_prepare(e, "SELECT json FROM items WHERE session_id=? AND active=1", &s, err);
        if (measured != AH_OK)
            return measured;
        sqlite3_bind_text(s, 1, session->id, -1, SQLITE_TRANSIENT);
        int step;
        bytes = strlen(session->instructions) + strlen(session->summary) + 4096;
        while ((step = sqlite3_step(s)) == SQLITE_ROW) {
            (void)sqlite3_column_text(s, 0);
            bytes += (size_t)sqlite3_column_bytes(s, 0);
        }
        sqlite3_finalize(s);
        if (step != SQLITE_DONE)
            return ah_fail(err, AH_IO, "cannot measure context for maintenance");
    }
    bool changed = false;
    core_session view = *session;
    ah_code compacted = core_context_compact(e, &view, bytes, &changed, err);
    if (compacted != AH_OK) {
        free(*json);
        *json = NULL;
        return compacted;
    }
    if (changed) {
        free(*json);
        *json = NULL;
        rc = build_request(e, session, json, err);
    }
    if (rc == AH_OK &&
        strlen(*json) > e->config.max_context_bytes - e->context_policy.reserve_bytes) {
        free(*json);
        *json = NULL;
        return ah_fail(err, AH_LIMIT,
                       "protected context leaves insufficient reserved space; supply an explicit "
                       "checkpoint or a larger budget");
    }
    return rc;
}
