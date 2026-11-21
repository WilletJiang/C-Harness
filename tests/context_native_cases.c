#include "core_test.h"
#include "agent_fixture.h"
#include "core/core.h"
#include "ah/context.h"
#include "providers/stream.h"

static const char compact_item[] =
    "{\"type\":\"compaction\",\"id\":\"cmp_one\",\"encrypted_content\":\"OPAQUE_ONE\"}";
static const char next_call[] =
    "{\"type\":\"function_call\",\"id\":\"fc_two\",\"name\":\"test_tool\","
    "\"call_id\":\"call_2\",\"arguments\":\"{\\\"value\\\":3}\"}";
static const char native_end[] =
    "{\"status\":\"completed\",\"output\":[{\"type\":\"compaction\","
    "\"id\":\"cmp_two\",\"encrypted_content\":\"OPAQUE_TWO\"},{\"type\":\"message\","
    "\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"NATIVE_DONE\"}]}]}";

static int scalar(ah_engine *e, const char *sql) {
    sqlite3_stmt *s = NULL;
    assert(sqlite3_prepare_v2(e->db, sql, -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    int value = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return value;
}

static char *streamed_response(void) {
    ah_stream stream;
    ah_stream_init(&stream, 4096, 16384, NULL, NULL);
    const char *items[] = {compact_item, next_call};
    for (unsigned i = 0; i < 2; i++) {
        char wire[1024];
        int count = snprintf(
            wire, sizeof(wire),
            "data: {\"type\":\"response.output_item.done\",\"output_index\":%u,\"item\":%s}\n\n", i,
            items[i]);
        assert(count > 0 && (size_t)count < sizeof(wire));
        for (int j = 0; j < count; j++)
            assert(ah_stream_feed(&stream, wire + j, 1) == AH_OK);
    }
    const char terminal[] =
        "data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\","
        "\"output\":[]}}\n\n";
    assert(ah_stream_feed(&stream, terminal, strlen(terminal)) == AH_OK);
    char *response = NULL;
    assert(ah_stream_finish(&stream, &response) == AH_OK);
    assert(strstr(response, "OPAQUE_ONE") && strstr(response, "call_2"));
    ah_stream_destroy(&stream);
    return response;
}

static void enable(ah_engine *e, bool native) {
    ah_context_config policy;
    ah_context_config_init(&policy);
    policy.keep_recent_exchanges = 0;
    policy.compact_threshold_tokens = native ? 100 : 0;
    ah_error error = {0};
    ok(ah_context_enable(e, &policy, &error), &error);
}

static void capability_and_roundtrip(void) {
    char *streamed = streamed_response();
    for (unsigned mode = 0; mode < 3; mode++) {
        fixture f = {.answers = {mode == 2 ? call : native_end, streamed, native_end}};
        ah_engine *e = open_engine(&f, ":memory:", NULL);
        register_tool(e, &f, AH_EFFECT_READ, false);
        if (mode)
            enable(e, mode == 2);
        char sid[AH_ID_CAP];
        create_input(e, sid);
        ah_error error = {0};
        ah_code code = ah_run(e, sid, &error);
        if (mode < 2) {
            assert(code == AH_PROTOCOL && !f.executions);
            assert(scalar(e, "SELECT COUNT(*) FROM context_checkpoints") == 0);
            assert(
                scalar(
                    e,
                    "SELECT COUNT(*) FROM items WHERE json_extract(json,'$.type')='compaction'") ==
                0);
        } else {
            ok(code, &error);
            assert(f.executions == 2 && f.calls == 3);
            assert(strstr(f.last_request, "OPAQUE_ONE") && strstr(f.last_request, "call_2"));
            assert(!strstr(f.last_request, "call_1"));
            assert(strstr(f.last_request, "context_management"));
            core_session session;
            ok(core_load(e, sid, &session, &error), &error);
            char *request = NULL;
            ok(core_request(e, &session, &request, &error), &error);
            assert(strstr(request, "OPAQUE_TWO") && !strstr(request, "OPAQUE_ONE"));
            assert(strstr(request, "first turn") && strstr(request, "NATIVE_DONE"));
            assert(!strstr(request, "call_2"));
            assert(scalar(e, "SELECT COUNT(*) FROM operations WHERE status='done'") == 2);
            assert(scalar(e, "SELECT COUNT(*) FROM items WHERE active=1 AND "
                             "json_extract(json,'$.type')='compaction'") == 1);
            free(request);
            bool changed = false;
            ok(core_context_compact(e, &session, 2000000, &changed, &error), &error);
            ok(core_request(e, &session, &request, &error), &error);
            assert(changed && strstr(request, "OPAQUE_TWO"));
            free(request);
            core_session_free(&session);
        }
        ah_close(e);
        free(f.last_request);
    }
    free(streamed);
}

static void rollback_and_validation(void) {
    fixture f = {0};
    ah_engine *e = open_engine(&f, ":memory:", NULL);
    register_tool(e, &f, AH_EFFECT_READ, false);
    enable(e, true);
    char sid[AH_ID_CAP];
    create_input(e, sid);
    ah_error error = {0};
    ok(core_add_item(e, sid, 1,
                     "{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_"
                     "text\",\"text\":\"old\"}]}",
                     &error),
       &error);
    core_session session;
    ok(core_load(e, sid, &session, &error), &error);
    const char empty[] = "{\"status\":\"completed\",\"output\":[{\"type\":\"compaction\","
                         "\"encrypted_content\":\"\"}]}";
    assert(core_accept_response(e, &session, empty, &error) == AH_PROTOCOL);
    ok(core_sql(e,
                "CREATE TRIGGER fail_archive BEFORE INSERT ON context_archive_items "
                "BEGIN SELECT RAISE(ABORT,'injected archive fault'); END",
                &error),
       &error);
    int before = scalar(e, "SELECT COUNT(*) FROM items");
    char *response = streamed_response();
    assert(core_accept_response(e, &session, response, &error) != AH_OK);
    assert(scalar(e, "SELECT COUNT(*) FROM items") == before);
    assert(scalar(e, "SELECT COUNT(*) FROM items WHERE active=1") == before);
    assert(scalar(e, "SELECT COUNT(*) FROM operations") == 0);
    assert(scalar(e, "SELECT COUNT(*) FROM context_checkpoints") == 0);
    bool changed = false;
    assert(core_context_compact(e, &session, 2000000, &changed, &error) != AH_OK && !changed);
    assert(scalar(e, "SELECT COUNT(*) FROM context_archive_items") == 0);
    assert(scalar(e, "SELECT COUNT(*) FROM context_checkpoints") == 0);
    assert(scalar(e, "SELECT COUNT(*) FROM items WHERE active=1") == before);
    ok(core_sql(e, "DROP TRIGGER fail_archive", &error), &error);
    ok(core_context_compact(e, &session, 2000000, &changed, &error), &error);
    assert(changed && scalar(e, "SELECT COUNT(*) FROM context_checkpoints") == 1);
    ok(core_accept_response(e, &session, response, &error), &error);
    before = scalar(e, "SELECT COUNT(*) FROM context_checkpoints");
    assert(core_context_compact(e, &session, 2000000, &changed, &error) == AH_BUSY && !changed);
    assert(scalar(e, "SELECT COUNT(*) FROM context_checkpoints") == before);
    free(response);
    core_session_free(&session);
    ah_close(e);
}

static void human_reply_fork(void) {
    const char *names[] = {"ask", "harness_ask", "wait", "harness_wait"};
    for (unsigned kind = 0; kind < 4; kind++) {
        const char *args = kind < 2 ? "{\"question\":\"Preserve the question\"}"
                                    : "{\"reason\":\"Preserve the wait\"}";
        char *answer = af_call(names[kind], "human", args);
        fixture f = {.answers = {answer, done}};
        ah_engine *e = open_engine(&f, ":memory:", NULL);
        enable(e, true);
        char sid[AH_ID_CAP];
        create_input(e, sid);
        ah_error error = {0};
        ok(ah_run(e, sid, &error), &error);
        char *operation = pending(e, sid);
        ok(ah_reply(e, sid, operation, "{\"answer\":\"human-919\"}", &error), &error);
        free(operation);
        ok(ah_run(e, sid, &error), &error);
        assert(strstr(f.last_request, "human-919") && strstr(f.last_request, "Preserve the"));
        for (unsigned native = 0; native < 2; native++) {
            char fork[AH_ID_CAP];
            ok(ah_fork(e, sid, fork, &error), &error);
            core_session session;
            ok(core_load(e, fork, &session, &error), &error);
            if (native) {
                uint64_t after = 0;
                ok(core_begin(e, &after, &error), &error);
                ok(core_context_native(e, fork, INT64_MAX, &error), &error);
                ok(core_commit(e, after, &error), &error);
            } else {
                bool changed = false;
                ok(core_context_compact(e, &session, 2000000, &changed, &error), &error);
                assert(changed);
            }
            char *request = NULL;
            ok(core_request(e, &session, &request, &error), &error);
            assert(strstr(request, "human-919") && strstr(request, "Preserve the"));
            assert(strstr(request, "first turn"));
            free(request);
            core_session_free(&session);
        }
        ah_close(e);
        free(f.last_request);
        free(answer);
    }
}

static void first_native_checkpoint(void) {
    /* A first-response native checkpoint can have no inactive input at all. */
    fixture f = {.answers = {native_end}};
    ah_engine *e = open_engine(&f, ":memory:", NULL);
    enable(e, true);
    char sid[AH_ID_CAP], *request = NULL;
    create_input(e, sid);
    ah_error error = {0};
    ok(ah_run(e, sid, &error), &error);
    assert(!scalar(e, "SELECT COUNT(*) FROM items WHERE active=0"));
    core_session session;
    ok(core_load(e, sid, &session, &error), &error);
    ok(core_request(e, &session, &request, &error), &error);
    const char *user = strstr(request, "first turn"), *note = strstr(request, "Provider-compacted"),
               *item = strstr(request, "OPAQUE_TWO");
    assert(user && note && item && user < note && note < item);
    free(request);
    core_session_free(&session);
    ah_close(e);
    free(f.last_request);
}

void context_native_cases(void) {
    first_native_checkpoint();
    capability_and_roundtrip();
    rollback_and_validation();
    human_reply_fork();
}
