#include "work.h"
#include "flow_vm.h"
#include <stdio.h>

/* Read each committed wave once. Cold replay also needs the original arguments
 * and names to verify the script against previously dispatched calls. */
static ah_code history(ah_engine *e, const ah_tool_call *call, yyjson_mut_doc *doc,
                       yyjson_mut_val *rows, core_flow_cell *cell, bool live, ah_error *error) {
    sqlite3_stmt *s = NULL;
    ah_code code = core_prepare(
        e,
        "SELECT w.key,o.name,o.arguments,o.result,o.is_error,o.status,o.seq FROM work_calls w "
        "JOIN operations o ON o.id=w.operation_id WHERE w.parent_id=? AND w.session_id=? AND "
        "o.seq>? ORDER BY "
        "o.seq",
        &s, error);
    if (code != AH_OK)
        return code;
    sqlite3_bind_text(s, 1, call->operation_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, call->session_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 3, cell->last_seq);
    int step;
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        const char *key = (const char *)sqlite3_column_text(s, 0),
                   *name = (const char *)sqlite3_column_text(s, 1);
        if (strcmp((const char *)sqlite3_column_text(s, 5), "done")) {
            code = ah_fail(error, AH_BUSY, "Unsettled flow child");
            break;
        }
        yyjson_mut_val *row = yyjson_mut_obj(doc);
        yyjson_mut_obj_add(rows, yyjson_mut_strcpy(doc, key), row);
        bool failed = sqlite3_column_int(s, 4) != 0;
        if (live) {
            yyjson_mut_obj_add_str(doc, row, "status", "done");
            yyjson_mut_obj_add_bool(doc, row, "is_error", failed);
        } else {
            yyjson_mut_obj_add_strcpy(doc, row, "name", name);
            yyjson_mut_obj_add_bool(doc, row, "failed", failed);
        }
        for (int i = 2; i <= 3; ++i) {
            const char *json = (const char *)sqlite3_column_text(s, i);
            if (!json ||
                (cell->history_bytes += (size_t)sqlite3_column_bytes(s, i)) > 12 * 1024 * 1024) {
                code = ah_fail(error, AH_LIMIT, "Flow history exceeds its limit");
                break;
            }
            if (live && i == 2)
                continue;
            yyjson_doc *part = yyjson_read(json, strlen(json), 0);
            bool ok =
                part && yyjson_mut_obj_add_val(doc, row, i == 2 ? "args" : "result",
                                               yyjson_val_mut_copy(doc, yyjson_doc_get_root(part)));
            yyjson_doc_free(part);
            if (!ok) {
                code = ah_fail(error, AH_IO, "Cannot read flow child receipt");
                break;
            }
        }
        if (code != AH_OK)
            break;
        cell->issued++;
        cell->last_seq = sqlite3_column_int64(s, 6);
    }
    if (code == AH_OK && step != SQLITE_DONE)
        code = ah_fail(error, AH_IO, "Cannot read flow history");
    sqlite3_finalize(s);
    return code;
}

static bool callable(ah_engine *e, const char *name) {
    return name && strcmp(name, "flow") &&
           (core_find_tool(e, name) || !strcmp(name, "read_result"));
}

static ah_code accept(ah_engine *e, const ah_tool_call *call, yyjson_val *reply,
                      yyjson_val *previous, yyjson_mut_val *names, unsigned issued,
                      ah_tool_result *result, ah_error *err) {
    yyjson_val *value = yyjson_obj_get(reply, "value"), *calls = yyjson_obj_get(reply, "calls");
    const char *failure = yyjson_get_str(yyjson_obj_get(reply, "error"));
    if (failure)
        return ah_fail(err, AH_INVALID, "%s", failure);
    if (value) {
        result->json = ah_json_write(value);
        if (!result->json)
            return ah_fail(err, AH_IO, "Cannot encode flow result");
        if (strlen(result->json) <= call->output_limit)
            return AH_OK;
        free(result->json);
        result->json = NULL;
        return ah_fail(err, AH_LIMIT, "Flow result exceeds output budget");
    }
    size_t n = yyjson_arr_size(calls);
    if (!n || issued + n > 32)
        return ah_fail(err, AH_LIMIT, "Invalid flow child count");
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(doc), *cp = yyjson_mut_obj(doc),
                   *wait = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_val *keys = yyjson_mut_obj_add_arr(doc, wait, "calls");
    yyjson_mut_val *waves = previous ? yyjson_val_mut_copy(doc, previous) : yyjson_mut_arr(doc);
    yyjson_mut_arr_add_uint(doc, waves, n);
    yyjson_mut_obj_add_val(doc, cp, "waves", waves);
    yyjson_mut_obj_add_val(doc, cp, "names", yyjson_mut_val_mut_copy(doc, names));
    yyjson_mut_obj_add_val(doc, root, "checkpoint", cp);
    yyjson_mut_obj_add_val(doc, root, "wait", wait);
    yyjson_mut_obj_add_val(doc, root, "calls", yyjson_val_mut_copy(doc, calls));
    ah_code code = AH_OK;
    for (size_t i = 0; i < n; ++i) {
        yyjson_val *c = yyjson_arr_get(calls, i), *args = yyjson_obj_get(c, "arguments");
        char key[16];
        snprintf(key, sizeof key, "c%u", issued + (unsigned)i);
        yyjson_val *tool = yyjson_obj_get(c, "tool");
        const char *name = yyjson_get_str(tool);
        if (!yyjson_equals_str(yyjson_obj_get(c, "key"), key) || !callable(e, name) ||
            strlen(name) != yyjson_get_len(tool) || !yyjson_is_obj(args)) {
            code = ah_fail(err, AH_INVALID, "Flow requested an invalid tool call");
            break;
        }
        yyjson_mut_arr_add_strcpy(doc, keys, key);
    }
    if (code == AH_OK) {
        result->continuation_json = ah_mut_write(doc);
        if (!result->continuation_json ||
            strlen(result->continuation_json) > CORE_WORK_JSON_LIMIT) {
            free(result->continuation_json);
            result->continuation_json = NULL;
            code = ah_fail(err, AH_LIMIT, "Flow batch exceeds continuation budget");
        }
    }
    ah_mut_doc_free(doc);
    return code;
}

static yyjson_mut_val *tool_names(ah_engine *e, yyjson_mut_doc *doc, yyjson_val *checkpoint) {
    yyjson_val *saved = yyjson_obj_get(checkpoint, "names");
    if (saved)
        return yyjson_val_mut_copy(doc, saved);
    yyjson_mut_val *names = yyjson_mut_arr(doc);
    for (size_t i = 0; i < e->tool_count; i++)
        if (callable(e, e->tools[i].name))
            yyjson_mut_arr_add_str(doc, names, e->tools[i].name);
    yyjson_mut_arr_add_str(doc, names, "read_result");
    return names;
}

static ah_code phase(ah_engine *e, const ah_tool_call *call, core_flow_cell *cell, bool live,
                     ah_tool_result *result, ah_error *err) {
    yyjson_doc *cp = yyjson_read(call->checkpoint_json, strlen(call->checkpoint_json), 0);
    yyjson_val *checkpoint = yyjson_doc_get_root(cp), *waves = yyjson_obj_get(checkpoint, "waves");
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *names = tool_names(e, doc, checkpoint);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_val *rows = yyjson_mut_obj_add_obj(doc, root, live ? "calls" : "history");
    char *output = NULL;
    yyjson_doc *args = NULL;
    if (!live) {
        args = yyjson_read(call->arguments_json, strlen(call->arguments_json), 0);
        yyjson_mut_obj_add_val(doc, root, "names", names);
        yyjson_mut_obj_add_val(doc, root, "waves",
                               waves ? yyjson_val_mut_copy(doc, waves) : yyjson_mut_arr(doc));
        yyjson_mut_obj_add_val(
            doc, root, "script",
            yyjson_val_mut_copy(doc, yyjson_obj_get(yyjson_doc_get_root(args), "script")));
    }
    ah_code code = history(e, call, doc, rows, cell, live, err);
    char *input = code == AH_OK ? ah_mut_write(doc) : NULL;
    if (code == AH_OK)
        code = input ? core_flow_exchange(cell, call, input, &output, err)
                     : ah_fail(err, AH_IO, "Missing flow input");
    yyjson_doc *reply = output ? yyjson_read(output, strlen(output), 0) : NULL;
    if (code == AH_OK)
        code = accept(e, call, yyjson_doc_get_root(reply), waves, names, cell->issued, result, err);
    free(output);
    free(input);
    yyjson_doc_free(reply);
    yyjson_doc_free(args);
    ah_mut_doc_free(doc);
    yyjson_doc_free(cp);
    return code;
}

static ah_code execute(void *context, const ah_tool_call *call, ah_tool_result *result,
                       ah_error *err) {
    ah_engine *e = context;
    core_flow_cell *cell = core_flow_lookup(e, call);
    bool live = cell && cell->next_generation;
    ah_code code = cell ? AH_OK : core_flow_start(e, call->operation_id, &cell, err);
    if (code == AH_OK)
        code = phase(e, call, cell, live, result, err);
    if (live && cell->disconnected) {
        /* A dying VM cannot have dispatched a new tool: the owner first has
         * to accept and commit the entire batch returned by the helper. */
        core_flow_drop(e, cell);
        cell = NULL;
        code = core_flow_start(e, call->operation_id, &cell, err);
        if (code == AH_OK)
            code = phase(e, call, cell, false, result, err);
    }
    if (cell) {
        if (code == AH_OK && result->continuation_json)
            cell->next_generation = call->generation + 1;
        else
            core_flow_drop(e, cell);
    }
    if (code != AH_OK && code != AH_CANCELLED && code != AH_UNCERTAIN) {
        result->json = core_object_string("error", err->message);
        result->is_error = true;
        if (result->json)
            return AH_OK;
    }
    return code;
}

ah_code core_flow_register(ah_engine *e, ah_error *error) {
    static const ah_work_config work = {.version = 1, .replay = AH_REPLAY_SAFE};
    const ah_tool tool = {
        .name = "flow",
        .description =
            "Run async JavaScript. await tools.NAME(args) calls session tools with normal "
            "approvals. Use Promise.all for independent edits or reads; await edits before "
            "tools.test({}). Return only the needed value. No OS/network APIs.",
        .parameters_json =
            "{\"type\":\"object\",\"properties\":{\"script\":{\"type\":\"string\",\"minLength\":1,"
            "\"maxLength\":32768}},\"required\":[\"script\"],\"additionalProperties\":false}",
        .effect = AH_EFFECT_READ,
        .execute = execute,
        .context = e,
        .work = &work};
    return ah_register_tool(e, &tool, error);
}
