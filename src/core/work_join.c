#include "work.h"

yyjson_mut_val *core_work_value(yyjson_mut_doc *doc, const core_operation *op) {
    yyjson_mut_val *value = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, value, "operation_id", op->id);
    yyjson_mut_obj_add_strcpy(doc, value, "name", op->name);
    yyjson_mut_obj_add_strcpy(doc, value, "status", op->status);
    yyjson_mut_obj_add_bool(doc, value, "is_error", op->is_error);
    yyjson_mut_obj_add_uint(doc, value, "result_format", op->result_format);
    if (op->result) {
        yyjson_doc *result = yyjson_read(op->result, strlen(op->result), 0);
        yyjson_mut_val *copy =
            result ? yyjson_val_mut_copy(doc, yyjson_doc_get_root(result)) : NULL;
        yyjson_doc_free(result);
        if (!copy || !yyjson_mut_obj_add_val(doc, value, "result", copy))
            return NULL;
    } else
        yyjson_mut_obj_add_null(doc, value, "result");
    return value;
}

static ah_code child_operation(ah_engine *e, const char *sid, const char *parent, const char *key,
                               core_operation *operation, ah_error *error) {
    sqlite3_stmt *row = NULL;
    ah_code code = core_prepare(e,
                                "SELECT operation_id FROM work_calls "
                                "WHERE session_id=? AND parent_id=? AND key=?",
                                &row, error);
    if (code == AH_OK) {
        sqlite3_bind_text(row, 1, sid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(row, 2, parent, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(row, 3, key, -1, SQLITE_TRANSIENT);
        int step = sqlite3_step(row);
        if (step == SQLITE_ROW)
            code = core_get_operation(e, sid, (const char *)sqlite3_column_text(row, 0), operation,
                                      error);
        else
            code = ah_fail(error, step == SQLITE_DONE ? AH_INVALID : AH_IO,
                           "wait references an unavailable child key");
    }
    sqlite3_finalize(row);
    return code;
}

ah_code core_work_children_ready(ah_engine *e, const char *sid, const char *parent,
                                 yyjson_val *wait, bool *ready, char **wake, ah_error *error) {
    *ready = false;
    *wake = NULL;
    yyjson_val *keys = yyjson_obj_get(wait, "calls");
    if (!keys)
        return AH_OK;
    size_t count = yyjson_arr_size(keys);
    if (!yyjson_is_arr(keys) || !count || count > 32)
        return ah_fail(error, AH_INVALID, "wait calls must contain 1 to 32 child keys");
    const char *policy = yyjson_get_str(yyjson_obj_get(wait, "policy"));
    if (!policy)
        policy = "all";
    if (strcmp(policy, "all") && strcmp(policy, "any") && strcmp(policy, "fail_fast"))
        return ah_fail(error, AH_INVALID, "unknown child wait policy");
    yyjson_mut_doc *doc = ah_mut_doc_new();
    if (!doc)
        return ah_fail(error, AH_IO, "out of memory reading children");
    yyjson_mut_val *root = yyjson_mut_obj(doc), *calls = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "reason", "calls");
    yyjson_mut_obj_add_strcpy(doc, root, "policy", policy);
    yyjson_mut_obj_add_val(doc, root, "calls", calls);
    ah_code code = AH_OK;
    unsigned done = 0;
    bool failed = false;
    for (size_t index = 0; index < count && code == AH_OK; index++) {
        const char *key = yyjson_get_str(yyjson_arr_get(keys, index));
        if (!key || !*key || strlen(key) > 256) {
            code = ah_fail(error, AH_INVALID, "bounded child wait key required");
            break;
        }
        for (size_t prior = 0; prior < index; prior++) {
            const char *other = yyjson_get_str(yyjson_arr_get(keys, prior));
            if (!strcmp(key, other))
                code = ah_fail(error, AH_INVALID, "duplicate child wait key");
        }
        if (code != AH_OK)
            break;
        core_operation operation;
        code = child_operation(e, sid, parent, key, &operation, error);
        if (code != AH_OK)
            break;
        bool terminal = !strcmp(operation.status, "done");
        done += terminal;
        failed |= terminal && operation.is_error;
        yyjson_mut_val *value = core_work_value(doc, &operation);
        if (!value || !yyjson_mut_obj_add_val(doc, calls, key, value))
            code = ah_fail(error, AH_IO, "cannot encode child snapshot");
        core_operation_free(&operation);
    }
    if (code == AH_OK) {
        *ready = !strcmp(policy, "any")         ? done > 0
                 : !strcmp(policy, "fail_fast") ? failed || done == count
                                                : done == count;
        *wake = ah_mut_write(doc);
        if (!*wake)
            code = ah_fail(error, AH_IO, "out of memory encoding child wake");
    }
    ah_mut_doc_free(doc);
    return code;
}
