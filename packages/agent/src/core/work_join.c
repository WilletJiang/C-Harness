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

/* Use one indexed statement for the whole join. Polling needs status only;
 * claim also materializes a single snapshot directly in the caller's document. */
ah_code core_work_children_ready(ah_engine *e, const char *sid, const char *parent,
                                 yyjson_val *wait, bool *ready, yyjson_mut_doc *doc,
                                 yyjson_mut_val **values, ah_error *error) {
    *ready = false;
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
    if (doc)
        *values = yyjson_mut_obj(doc);
    sqlite3_stmt *row = NULL;
    ah_code code = core_prepare(
        e,
        "SELECT o.status,o.is_error,o.id,o.name,o.result_format,CASE WHEN ?4 THEN o.result END "
        "FROM work_calls w JOIN operations o ON o.id=w.operation_id WHERE w.session_id=?1 AND "
        "w.parent_id=?2 AND w.key=?3",
        &row, error);
    if (code != AH_OK)
        return code;
    sqlite3_bind_text(row, 1, sid, -1, SQLITE_STATIC);
    sqlite3_bind_text(row, 2, parent, -1, SQLITE_STATIC);
    sqlite3_bind_int(row, 4, doc != NULL);
    unsigned done = 0;
    bool failed = false;
    for (size_t i = 0; i < count; i++) {
        yyjson_val *v = yyjson_arr_get(keys, i);
        const char *key = yyjson_get_str(v);
        if (!key || !*key || strlen(key) > 256 || strlen(key) != yyjson_get_len(v)) {
            code = ah_fail(error, AH_INVALID, "bounded child wait key required");
            break;
        }
        for (size_t j = 0; j < i; j++)
            if (yyjson_equals_str(yyjson_arr_get(keys, j), key))
                code = ah_fail(error, AH_INVALID, "duplicate child wait key");
        if (code != AH_OK)
            break;
        sqlite3_bind_text(row, 3, key, -1, SQLITE_STATIC);
        int step = sqlite3_step(row);
        if (step != SQLITE_ROW) {
            code = ah_fail(error, step == SQLITE_DONE ? AH_INVALID : AH_IO,
                           "wait references an unavailable child key");
            break;
        }
        char *status = (char *)sqlite3_column_text(row, 0);
        bool terminal = !strcmp(status, "done"), is_error = sqlite3_column_int(row, 1) != 0;
        done += terminal;
        failed |= terminal && is_error;
        if (doc) {
            core_operation op = {.id = (char *)sqlite3_column_text(row, 2),
                                 .name = (char *)sqlite3_column_text(row, 3),
                                 .status = status,
                                 .is_error = is_error,
                                 .result_format = sqlite3_column_int(row, 4),
                                 .result = (char *)sqlite3_column_text(row, 5)};
            yyjson_mut_val *item = core_work_value(doc, &op);
            if (!item || !yyjson_mut_obj_add_val(doc, *values, key, item)) {
                code = ah_fail(error, AH_IO, "cannot encode child snapshot");
                break;
            }
        }
        sqlite3_reset(row);
    }
    sqlite3_finalize(row);
    if (code == AH_OK)
        *ready = !strcmp(policy, "any")         ? done > 0
                 : !strcmp(policy, "fail_fast") ? failed || done == count
                                                : done == count;
    return code;
}
