#include "core_test.h"
#include "agent_fixture.h"
#include "core/core.h"
#include "ah/context.h"

static int scalar(ah_engine *e, const char *query) {
    sqlite3_stmt *s = NULL;
    assert(sqlite3_prepare_v2(e->db, query, -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    int value = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return value;
}

static ah_code large_result(void *context, const ah_tool_call *call, ah_tool_result *result,
                            ah_error *error) {
    (void)call;
    (void)error;
    fixture *f = context;
    char payload[2600];
    memset(payload, 'x', sizeof(payload) - 1);
    memcpy(payload, "中文\"\\\n", strlen("中文\"\\\n"));
    payload[sizeof(payload) - 1] = 0;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_uint(doc, root, "round", ++f->executions);
    yyjson_mut_obj_add_str(doc, root, "payload", payload);
    result->json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    assert(result->json);
    return AH_OK;
}

static void complete_exchanges(const char *request) {
    yyjson_doc *doc = yyjson_read(request, strlen(request), 0);
    assert(doc);
    yyjson_val *input = yyjson_obj_get(yyjson_doc_get_root(doc), "input"), *item;
    size_t i, count;
    yyjson_arr_foreach(input, i, count, item) {
        const char *id = yyjson_get_str(yyjson_obj_get(item, "call_id"));
        if (!id)
            continue;
        unsigned calls = 0, receipts = 0;
        size_t j, n;
        yyjson_val *other;
        yyjson_arr_foreach(input, j, n, other) {
            if (!yyjson_equals_str(yyjson_obj_get(other, "call_id"), id))
                continue;
            calls += yyjson_equals_str(yyjson_obj_get(other, "type"), "function_call");
            receipts += yyjson_equals_str(yyjson_obj_get(other, "type"), "function_call_output");
        }
        assert(calls == 1 && receipts == 1);
    }
    yyjson_doc_free(doc);
}

static void recall_exact(ah_engine *e, const char *sid, const char *fork) {
    sqlite3_stmt *s = NULL;
    assert(sqlite3_prepare_v2(
               e->db,
               "SELECT a.checkpoint_id,i.seq,i.json FROM context_archive_items a JOIN items i "
               "ON i.seq=a.item_seq WHERE json_extract(i.json,'$.call_id')='large_1' "
               "AND json_extract(i.json,'$.type')='function_call_output' LIMIT 1",
               -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    char *checkpoint = strdup((const char *)sqlite3_column_text(s, 0));
    sqlite3_int64 seq = sqlite3_column_int64(s, 1);
    char *expected = strdup((const char *)sqlite3_column_text(s, 2));
    sqlite3_finalize(s);
    assert(checkpoint && expected);
    char *assembled = calloc(strlen(expected) + 1, 1);
    size_t offset = 0;
    unsigned pages = 0;
    bool more = true;
    while (more) {
        char args[256];
        snprintf(args, sizeof(args),
                 "{\"checkpoint\":\"%s\",\"after_seq\":%lld,\"offset\":%zu,\"limit\":65536}",
                 checkpoint, (long long)seq - 1, offset);
        ah_tool_call call = {.session_id = pages % 2 ? fork : sid,
                             .arguments_json = args,
                             .output_limit = 1024,
                             .model_output_limit = 1024};
        ah_tool_result result = {0};
        ah_error error = {0};
        ok(core_context_recall(e, &call, &result, &error), &error);
        assert(result.json && strlen(result.json) <= 1024);
        yyjson_doc *doc = yyjson_read(result.json, strlen(result.json), 0);
        yyjson_val *root = yyjson_doc_get_root(doc), *content = yyjson_obj_get(root, "content");
        size_t length = yyjson_get_len(content);
        assert(length && offset + length <= strlen(expected));
        memcpy(assembled + offset, yyjson_get_str(content), length);
        offset += length;
        assert(yyjson_get_uint(yyjson_obj_get(root, "next_offset")) == offset);
        assert(yyjson_get_sint(yyjson_obj_get(root, "next_seq")) == seq);
        more = yyjson_get_bool(yyjson_obj_get(root, "more"));
        yyjson_doc_free(doc);
        free(result.json);
        assert(++pages < 1000);
    }
    assert(pages > 1 && !strcmp(assembled, expected));
    char stranger[AH_ID_CAP], args[256];
    ah_error error = {0};
    const char *unicode = strstr(expected, "中文");
    assert(unicode);
    snprintf(args, sizeof(args), "{\"checkpoint\":\"%s\",\"after_seq\":%lld,\"offset\":%zu}",
             checkpoint, (long long)seq - 1, (size_t)(unicode - expected) + 1);
    ah_tool_call invalid = {.session_id = sid, .arguments_json = args, .output_limit = 1024};
    ah_tool_result rejected = {0};
    assert(core_context_recall(e, &invalid, &rejected, &error) == AH_INVALID && !rejected.json);
    snprintf(args, sizeof(args),
             "{\"checkpoint\":\"%s\",\"after_seq\":%lld,\"offset\":9007199254740991}", checkpoint,
             (long long)seq - 1);
    assert(core_context_recall(e, &invalid, &rejected, &error) == AH_INVALID && !rejected.json);
    ok(ah_session_create(e, "mock", "", stranger, &error), &error);
    snprintf(args, sizeof(args), "{\"checkpoint\":\"%s\"}", checkpoint);
    ah_tool_call call = {.session_id = stranger, .arguments_json = args, .output_limit = 1024};
    ah_tool_result result = {0};
    assert(core_context_recall(e, &call, &result, &error) == AH_NOT_FOUND && !result.json);
    free(assembled);
    free(expected);
    free(checkpoint);
}

static void bounded_loop(bool enabled) {
    fixture f = {0};
    char *answers[8];
    answers[0] = af_call("ask", "human", "{\"question\":\"Keep user choice exactly?\"}");
    for (unsigned i = 1; i < 7; i++) {
        char id[32];
        snprintf(id, sizeof(id), "large_%u", i);
        answers[i] = af_call("large", id, "{}");
    }
    answers[7] = af_text("FIXED_ACCEPTANCE");
    for (unsigned i = 0; i < 8; i++)
        f.answers[i] = answers[i];
    ah_config config;
    ah_config_init(&config);
    config.max_context_bytes = 12000;
    config.max_model_tool_bytes = 0;
    char database[] = "/tmp/ah-context-policy-XXXXXX";
    if (enabled) {
        int fd = mkstemp(database);
        assert(fd >= 0 && close(fd) == 0);
    }
    ah_engine *e = open_engine(&f, enabled ? database : ":memory:", &config);
    ah_error error = {0};
    ah_tool tool = {.name = "large",
                    .description = "Bounded exact result",
                    .parameters_json = "{\"type\":\"object\",\"additionalProperties\":false}",
                    .effect = AH_EFFECT_READ,
                    .execute = large_result,
                    .context = &f};
    ok(ah_register_tool(e, &tool, &error), &error);
    ah_context_config policy = {.trigger_bytes = 9000,
                                .target_bytes = 6000,
                                .reserve_bytes = 1000,
                                .keep_recent_exchanges = 1};
    if (enabled)
        ok(ah_context_enable(e, &policy, &error), &error);
    char sid[AH_ID_CAP];
    create_input(e, sid);
    ok(ah_run(e, sid, &error), &error);
    char *operation = pending(e, sid);
    ok(ah_reply(e, sid, operation, "{\"choice\":\"keep-4839\"}", &error), &error);
    free(operation);
    ah_code code = ah_run(e, sid, &error);
    if (enabled) {
        ok(code, &error);
        state_is(e, sid, "completed");
        assert(f.calls == 8 && f.executions == 6);
        assert(scalar(e, "SELECT COUNT(*) FROM context_checkpoints") > 0);
        assert(strstr(f.last_request, "first turn") && strstr(f.last_request, "keep-4839"));
        const char *note = strstr(f.last_request, "Latest checkpoint");
        const char *choice = strstr(f.last_request, "Keep user choice exactly?");
        assert(note && choice && strstr(f.last_request, "first turn") < note && choice < note);
        assert(strstr(f.last_request, "large_6") && !strstr(f.last_request, "large_1"));
        complete_exchanges(f.last_request);
        char fork[AH_ID_CAP];
        ok(ah_fork(e, sid, fork, &error), &error);
        recall_exact(e, sid, fork);
        policy.keep_recent_exchanges = 0;
        ok(ah_context_enable(e, &policy, &error), &error);
        core_session view;
        ok(core_load(e, fork, &view, &error), &error);
        bool changed = false;
        ok(core_context_compact(e, &view, 30000, &changed, &error), &error);
        assert(changed);
        char *request = NULL;
        ok(core_request(e, &view, &request, &error), &error);
        assert(strstr(request, "keep-4839") && strstr(request, "Keep user choice exactly?"));
        assert(strstr(request, "first turn") && strstr(request, "Latest checkpoint"));
        assert(strstr(request, "first turn") < strstr(request, "Latest checkpoint"));
        complete_exchanges(request);
        free(request);
        core_session_free(&view);
        ah_close(e);
        f.last_seq = 0;
        e = open_engine(&f, database, &config);
        ok(ah_register_tool(e, &tool, &error), &error);
        assert(!e->context_enabled && e->context_history);
        ok(core_load(e, sid, &view, &error), &error);
        ok(core_request(e, &view, &request, &error), &error);
        assert(strstr(request, "\"name\":\"recall\"") && strstr(request, "Latest checkpoint"));
        assert(strstr(request, "first turn") &&
               strstr(request, "first turn") < strstr(request, "Latest checkpoint"));
        free(request);
        core_session_free(&view);
        ok(ah_context_enable(e, &policy, &error), &error);
        recall_exact(e, sid, fork); /* Checkpoint ownership survives engine reopen. */
    } else {
        assert(code == AH_LIMIT && f.calls < 8);
        state_is(e, sid, "stopped");
    }
    ah_close(e);
    if (enabled) {
        char lock[256];
        snprintf(lock, sizeof(lock), "%s.lock", database);
        assert(unlink(database) == 0 && unlink(lock) == 0);
    }
    free(f.last_request);
    for (unsigned i = 0; i < 8; i++)
        free(answers[i]);
}

static void checkpoint_pages(void) {
    fixture f = {0};
    ah_engine *e = open_engine(&f, ":memory:", NULL);
    char sid[AH_ID_CAP];
    create_input(e, sid);
    char summary[251];
    memset(summary, 'x', sizeof(summary) - 1);
    summary[sizeof(summary) - 1] = 0;
    ah_error error = {0};
    for (unsigned i = 0; i < 24; i++) {
        char *sql = sqlite3_mprintf("INSERT INTO context_checkpoints VALUES('page-%u',%Q,%Q,0,0);"
                                    "INSERT INTO context_checkpoint_refs VALUES(%Q,'page-%u')",
                                    i, sid, summary, sid, i);
        ok(core_sql(e, sql, &error), &error);
        sqlite3_free(sql);
    }
    unsigned total = 0, pages = 0;
    uint64_t after = 0;
    for (;;) {
        char args[128];
        snprintf(args, sizeof(args), "{\"after_seq\":%llu}", (unsigned long long)after);
        ah_tool_call call = {.session_id = sid,
                             .arguments_json = args,
                             .output_limit = 1024,
                             .model_output_limit = 1024};
        ah_tool_result result = {0};
        ok(core_context_recall(e, &call, &result, &error), &error);
        assert(result.json && strlen(result.json) <= 1024);
        yyjson_doc *doc = yyjson_read(result.json, strlen(result.json), 0);
        yyjson_val *root = yyjson_doc_get_root(doc), *items = yyjson_obj_get(root, "checkpoints");
        unsigned count = (unsigned)yyjson_arr_size(items);
        uint64_t next = yyjson_get_uint(yyjson_obj_get(root, "next_seq"));
        yyjson_doc_free(doc);
        free(result.json);
        if (!count)
            break;
        assert(next > after && ++pages <= 24);
        after = next;
        total += count;
    }
    assert(total == 24 && pages > 1);
    ah_close(e);
}

void context_native_cases(void);
int main(void) {
    bounded_loop(false);
    bounded_loop(true);
    checkpoint_pages();
    context_native_cases();
    puts("context policy, recall and native compaction tests passed");
    return 0;
}
