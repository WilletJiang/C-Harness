#include "work.h"
#include <stdio.h>

ah_code core_work_calls_schema(ah_engine *e, ah_error *error) {
    return core_sql(
        e,
        "CREATE TABLE IF NOT EXISTS work_sessions("
        "session_id TEXT PRIMARY KEY REFERENCES sessions(id) ON DELETE CASCADE);"
        "CREATE TABLE IF NOT EXISTS work_calls("
        "session_id TEXT NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
        "operation_id TEXT PRIMARY KEY REFERENCES operations(id) ON DELETE CASCADE,"
        "parent_id TEXT REFERENCES operations(id) ON DELETE CASCADE,"
        "key TEXT NOT NULL,origin INTEGER NOT NULL CHECK(origin IN(1,2)),"
        "UNIQUE(session_id,parent_id,key));"
        "CREATE UNIQUE INDEX IF NOT EXISTS work_root_keys ON work_calls(session_id,key) "
        "WHERE parent_id IS NULL;"
        "CREATE INDEX IF NOT EXISTS work_children ON work_calls(parent_id);",
        error);
}

static bool same_json(const char *left, const char *right) {
    yyjson_doc *a = yyjson_read(left, strlen(left), 0);
    yyjson_doc *b = yyjson_read(right, strlen(right), 0);
    bool same = a && b && yyjson_equals(yyjson_doc_get_root(a), yyjson_doc_get_root(b));
    yyjson_doc_free(a);
    yyjson_doc_free(b);
    return same;
}

static ah_code existing_call(ah_engine *e, const char *sid, const char *parent, const char *key,
                             const char *name, const char *args, char id[AH_ID_CAP],
                             ah_error *error) {
    sqlite3_stmt *row = NULL;
    ah_code code =
        core_prepare(e,
                     "SELECT o.id,o.name,o.arguments FROM work_calls w JOIN operations o "
                     "ON o.id=w.operation_id WHERE w.session_id=? AND w.parent_id IS ? AND w.key=?",
                     &row, error);
    if (code == AH_OK) {
        sqlite3_bind_text(row, 1, sid, -1, SQLITE_TRANSIENT);
        if (parent)
            sqlite3_bind_text(row, 2, parent, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(row, 3, key, -1, SQLITE_TRANSIENT);
        int step = sqlite3_step(row);
        if (step == SQLITE_ROW) {
            bool same = !strcmp(name, (const char *)sqlite3_column_text(row, 1)) &&
                        same_json(args, (const char *)sqlite3_column_text(row, 2));
            if (same)
                snprintf(id, AH_ID_CAP, "%s", (const char *)sqlite3_column_text(row, 0));
            else
                code =
                    ah_fail(error, AH_INVALID, "work key reused with different tool or arguments");
        } else
            code = step == SQLITE_DONE ? AH_NOT_FOUND
                                       : ah_fail(error, AH_IO, "cannot inspect keyed work call");
    }
    sqlite3_finalize(row);
    return code;
}

static ah_code graph_limits(ah_engine *e, const char *sid, const char *parent, ah_error *error) {
    if (!parent)
        return AH_OK;
    core_operation operation;
    ah_code code = core_get_operation(e, sid, parent, &operation, error);
    if (code != AH_OK)
        return code;
    bool active = !strcmp(operation.status, "executing");
    core_operation_free(&operation);
    if (!active)
        return ah_fail(error, AH_INVALID, "child calls require an executing parent");
    sqlite3_stmt *row = NULL;
    code = core_prepare(
        e,
        "WITH RECURSIVE ancestry(id,depth) AS (SELECT ?1,0 UNION ALL "
        "SELECT w.parent_id,a.depth+1 FROM work_calls w JOIN ancestry a "
        "ON w.operation_id=a.id WHERE w.parent_id IS NOT NULL AND a.depth<9),"
        "tree(id) AS (SELECT id FROM ancestry WHERE depth=(SELECT MAX(depth) FROM ancestry) "
        "UNION ALL SELECT w.operation_id FROM work_calls w JOIN tree t ON w.parent_id=t.id) "
        "SELECT (SELECT MAX(depth) FROM ancestry),(SELECT COUNT(*) FROM tree),"
        "(SELECT COUNT(*) FROM work_calls WHERE parent_id=?1)",
        &row, error);
    if (code == AH_OK) {
        sqlite3_bind_text(row, 1, parent, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(row) != SQLITE_ROW)
            code = ah_fail(error, AH_IO, "cannot inspect work graph bounds");
        else if (sqlite3_column_int(row, 0) >= 8 || sqlite3_column_int(row, 1) >= 128 ||
                 sqlite3_column_int(row, 2) >= 32)
            code = ah_fail(error, AH_LIMIT, "work graph exceeds depth, tree, or child limit");
    }
    sqlite3_finalize(row);
    return code;
}

ah_code core_work_create_call(ah_engine *e, const char *sid, const char *parent, const char *key,
                              const char *name, const char *args, char id[AH_ID_CAP],
                              ah_error *error) {
    e->has_work = true;
    size_t complete = 0;
    if (!sid || !key || !*key || strlen(key) > 256 ||
        !ah_utf8_prefix(key, strlen(key), &complete) || complete != strlen(key) || !name ||
        !*name || strlen(name) > 64 || !args || strlen(args) > e->config.max_tool_arguments_bytes ||
        !ah_json_valid(args) || !id)
        return ah_fail(error, AH_INVALID, "bounded key, tool and JSON arguments required");
    ah_code code = existing_call(e, sid, parent, key, name, args, id, error);
    if (code != AH_NOT_FOUND)
        return code;
    code = graph_limits(e, sid, parent, error);
    core_tool *tool = core_find_tool(e, name);
    const char *schema = tool ? tool->schema : core_builtin_schema(name);
    if (code == AH_OK && !schema)
        code = ah_fail(error, AH_NOT_FOUND, "work tool is unavailable");
    if (code == AH_OK)
        code = core_id(id, error);
    sqlite3_stmt *row = NULL;
    if (code == AH_OK)
        code = core_prepare(
            e,
            "INSERT INTO operations(id,session_id,call_id,name,arguments,schema,effect,approval,"
            "status) VALUES(?1,?2,?1,?3,?4,?5,?6,?7,'pending')",
            &row, error);
    if (code == AH_OK) {
        sqlite3_bind_text(row, 1, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(row, 2, sid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(row, 3, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(row, 4, args, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(row, 5, schema, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(row, 6, tool ? tool->tool.effect : AH_EFFECT_READ);
        sqlite3_bind_int(row, 7, tool && tool->tool.requires_approval);
        code = core_done(e, row, error);
    }
    if (code == AH_OK)
        code = core_prepare(e, "INSERT INTO call_ids(session_id,call_id) VALUES(?,?)", &row, error);
    if (code == AH_OK) {
        sqlite3_bind_text(row, 1, sid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(row, 2, id, -1, SQLITE_TRANSIENT);
        code = core_done(e, row, error);
    }
    if (code == AH_OK)
        code = core_prepare(e,
                            "INSERT INTO work_calls(session_id,operation_id,parent_id,key,origin) "
                            "VALUES(?,?,?,?,?)",
                            &row, error);
    if (code == AH_OK) {
        sqlite3_bind_text(row, 1, sid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(row, 2, id, -1, SQLITE_TRANSIENT);
        if (parent)
            sqlite3_bind_text(row, 3, parent, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(row, 4, key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(row, 5, parent ? 2 : 1);
        code = core_done(e, row, error);
    }
    if (code == AH_OK && tool) {
        core_operation operation = {.id = id};
        code = core_work_seed(e, sid, &operation, tool, error);
    }
    yyjson_mut_doc *doc = code == AH_OK ? ah_mut_doc_new() : NULL;
    if (code == AH_OK && !doc)
        code = ah_fail(error, AH_IO, "out of memory recording work intent");
    if (code == AH_OK) {
        yyjson_mut_val *event = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, event);
        yyjson_mut_obj_add_strcpy(doc, event, "key", key);
        yyjson_mut_obj_add_strcpy(doc, event, "name", name);
        yyjson_mut_obj_add_strcpy(doc, event, "arguments", args);
        if (parent)
            yyjson_mut_obj_add_strcpy(doc, event, "parent_id", parent);
        char *json = ah_mut_write(doc);
        code = json ? core_event(e, sid, id, "work.created", json, error)
                    : ah_fail(error, AH_IO, "out of memory recording work intent");
        free(json);
    }
    ah_mut_doc_free(doc);
    return code;
}

ah_code core_work_children(ah_engine *e, const char *sid, const char *parent, uint64_t generation,
                           yyjson_val *calls, ah_error *error) {
    (void)generation; /* Keys belong to the operation, across all generations. */
    if (!calls)
        return AH_OK;
    if (!yyjson_is_arr(calls) || yyjson_arr_size(calls) > 32)
        return ah_fail(error, AH_INVALID, "calls must be an array of at most 32 keyed tools");
    size_t index, count;
    yyjson_val *call;
    yyjson_arr_foreach(calls, index, count, call) {
        const char *key = yyjson_get_str(yyjson_obj_get(call, "key"));
        const char *tool = yyjson_get_str(yyjson_obj_get(call, "tool"));
        yyjson_val *args = yyjson_obj_get(call, "arguments");
        if (!yyjson_is_obj(call) || !key || !tool || !args)
            return ah_fail(error, AH_INVALID, "child call requires key, tool and arguments");
        for (size_t prior = 0; prior < index; prior++) {
            const char *other = yyjson_get_str(yyjson_obj_get(yyjson_arr_get(calls, prior), "key"));
            if (!strcmp(key, other))
                return ah_fail(error, AH_INVALID, "duplicate child key in continuation");
        }
        char *json = ah_json_write(args), id[AH_ID_CAP];
        ah_code code = json ? core_work_create_call(e, sid, parent, key, tool, json, id, error)
                            : ah_fail(error, AH_IO, "out of memory encoding child arguments");
        free(json);
        if (code != AH_OK)
            return code;
    }
    return AH_OK;
}
