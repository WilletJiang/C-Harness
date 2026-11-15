#include "durable_fixture.h"
#include "ah/jobs.h"
#include "ah/work.h"

typedef struct {
    durable_fixture fixture;
    unsigned parent_calls, leaf_calls;
    bool park_leaf, fail_leaf, unknown_leaf;
} nested_fixture;

static ah_code leaf(void *context, const ah_tool_call *call, ah_tool_result *result,
                    ah_error *error) {
    (void)call;
    (void)error;
    nested_fixture *f = context;
    f->leaf_calls++;
    if (f->park_leaf)
        result->continuation_json =
            strdup("{\"checkpoint\":{\"phase\":1},\"wait\":{\"signal\":\"ready\"}}");
    else
        result->json = strdup("{\"leaf\":42}");
    result->is_error = f->fail_leaf;
    result->outcome_unknown = f->unknown_leaf;
    assert(result->json || result->continuation_json);
    return AH_OK;
}

static ah_code parent(void *context, const ah_tool_call *call, ah_tool_result *result,
                      ah_error *error) {
    (void)error;
    nested_fixture *f = context;
    f->parent_calls++;
    yyjson_doc *doc = yyjson_read(call->checkpoint_json, strlen(call->checkpoint_json), 0);
    assert(doc);
    unsigned phase = (unsigned)yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(doc), "phase"));
    yyjson_doc_free(doc);
    if (phase) {
        assert(call->wake_json);
        yyjson_doc *wake = yyjson_read(call->wake_json, strlen(call->wake_json), 0);
        assert(wake);
        yyjson_val *root = yyjson_doc_get_root(wake);
        assert(yyjson_equals_str(yyjson_obj_get(root, "reason"), "calls"));
        yyjson_val *child = yyjson_obj_get(yyjson_obj_get(root, "calls"), "stable-child");
        assert(yyjson_equals_str(yyjson_obj_get(child, "status"), "done"));
        assert(yyjson_get_bool(yyjson_obj_get(child, "is_error")) == f->fail_leaf);
        assert(yyjson_get_uint(yyjson_obj_get(yyjson_obj_get(child, "result"), "leaf")) == 42);
        yyjson_doc_free(wake);
    }
    if (phase < 2) {
        char continuation[512];
        snprintf(continuation, sizeof(continuation),
                 "{\"checkpoint\":{\"phase\":%u},\"calls\":[{\"key\":\"stable-child\","
                 "\"tool\":\"leaf\",\"arguments\":%s}],"
                 "\"wait\":{\"calls\":[\"stable-child\"],\"policy\":\"all\"}}",
                 phase + 1, phase ? "{\"b\":2,\"a\":1}" : "{\"a\":1,\"b\":2}");
        result->continuation_json = strdup(continuation);
        assert(result->continuation_json);
    } else {
        assert(phase == 2 && call->wake_json);
        result->json = strdup("{\"parent\":42}");
        result->is_error = f->fail_leaf;
        assert(result->json);
    }
    return AH_OK;
}

static void register_nested(nested_fixture *state, bool approval) {
    durable_fixture *f = &state->fixture;
    ah_work_config work = {.version = 1, .replay = AH_REPLAY_SAFE};
    ah_tool tool = {.name = "durable",
                    .description = "Repeat a stable child key",
                    .parameters_json = "{\"type\":\"object\"}",
                    .effect = AH_EFFECT_READ,
                    .execute = parent,
                    .context = state,
                    .work = &work};
    ah_error error = {0};
    df_ok(ah_register_tool(f->engine, &tool, &error), &error);
    tool.name = "leaf";
    tool.execute = leaf;
    tool.work = state->park_leaf ? &work : NULL;
    tool.requires_approval = approval;
    tool.effect = state->unknown_leaf ? AH_EFFECT_EXTERNAL : AH_EFFECT_READ;
    df_ok(ah_register_tool(f->engine, &tool, &error), &error);
}

static yyjson_doc *nested_snapshot(durable_fixture *f, const char *session, const char *operation) {
    char *json = NULL;
    ah_error error = {0};
    df_ok(ah_work_json(f->engine, session, operation, &json, &error), &error);
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    assert(doc);
    ah_free(json);
    return doc;
}

static void nested_key_reuse(bool approval, bool failure) {
    nested_fixture state = {.fail_leaf = failure};
    durable_fixture *f = &state.fixture;
    df_open(f, ":memory:", 1);
    register_nested(&state, approval);
    ah_error error = {0};
    char session[AH_ID_CAP], operation[AH_ID_CAP];
    df_ok(ah_session_create(f->engine, "mock", "", session, &error), &error);
    df_ok(ah_work_start(f->engine, session, "parent", "durable", "{}", operation, &error), &error);
    df_ok(ah_work_run(f->engine, session, &error), &error);
    if (approval) {
        df_state(f, session, AH_WAITING_APPROVAL);
        assert(state.parent_calls == 1 && state.leaf_calls == 0);
        yyjson_doc *doc = nested_snapshot(f, session, operation);
        yyjson_val *children = yyjson_obj_get(yyjson_doc_get_root(doc), "children");
        assert(yyjson_arr_size(children) == 1);
        yyjson_val *child = yyjson_arr_get(children, 0);
        const char *id = yyjson_get_str(yyjson_obj_get(child, "operation_id"));
        assert(id);
        df_ok(ah_decide(f->engine, session, id, true, &error), &error);
        yyjson_doc_free(doc);
        df_ok(ah_work_run(f->engine, session, &error), &error);
    }
    assert(f->models == 0 && state.parent_calls == 3 && state.leaf_calls == 1);
    yyjson_doc *doc = nested_snapshot(f, session, operation);
    yyjson_val *root = yyjson_doc_get_root(doc), *children = yyjson_obj_get(root, "children");
    assert(yyjson_equals_str(yyjson_obj_get(root, "status"), "done"));
    assert(yyjson_get_bool(yyjson_obj_get(root, "is_error")) == failure);
    assert(yyjson_get_uint(yyjson_obj_get(yyjson_obj_get(root, "result"), "parent")) == 42);
    assert(yyjson_arr_size(children) == 1);
    yyjson_val *child = yyjson_arr_get(children, 0);
    assert(yyjson_equals_str(yyjson_obj_get(child, "key"), "stable-child"));
    assert(yyjson_equals_str(yyjson_obj_get(child, "status"), "done"));
    yyjson_doc_free(doc);
    df_ok(ah_work_run(f->engine, session, &error), &error);
    assert(state.parent_calls == 3 && state.leaf_calls == 1);
    ah_close(f->engine);
}

static void cancel_parent_and_child(void) {
    nested_fixture state = {.park_leaf = true};
    durable_fixture *f = &state.fixture;
    df_open(f, ":memory:", 1);
    register_nested(&state, false);
    ah_error error = {0};
    char session[AH_ID_CAP], operation[AH_ID_CAP], child_id[AH_ID_CAP];
    df_ok(ah_session_create(f->engine, "mock", "", session, &error), &error);
    df_ok(ah_work_start(f->engine, session, "parent", "durable", "{}", operation, &error), &error);
    df_ok(ah_work_run(f->engine, session, &error), &error);
    df_state(f, session, AH_WAITING_WORK);
    assert(state.parent_calls == 1 && state.leaf_calls == 1);
    yyjson_doc *doc = nested_snapshot(f, session, operation);
    yyjson_val *children = yyjson_obj_get(yyjson_doc_get_root(doc), "children");
    assert(yyjson_arr_size(children) == 1);
    const char *id = yyjson_get_str(yyjson_obj_get(yyjson_arr_get(children, 0), "operation_id"));
    assert(id);
    snprintf(child_id, sizeof(child_id), "%s", id);
    yyjson_doc_free(doc);
    df_ok(ah_work_cancel(f->engine, session, operation, &error), &error);
    doc = nested_snapshot(f, session, operation);
    yyjson_val *root = yyjson_doc_get_root(doc);
    children = yyjson_obj_get(root, "children");
    assert(yyjson_equals_str(yyjson_obj_get(root, "status"), "done"));
    assert(yyjson_get_bool(yyjson_obj_get(root, "is_error")));
    assert(yyjson_equals_str(yyjson_obj_get(yyjson_arr_get(children, 0), "status"), "done"));
    assert(yyjson_get_bool(yyjson_obj_get(yyjson_arr_get(children, 0), "is_error")));
    yyjson_doc_free(doc);
    ah_signal_result delivery;
    df_ok(ah_signal(f->engine, session, child_id, "ready", "true", &delivery, &error), &error);
    assert(delivery == AH_SIGNAL_CLOSED);
    df_ok(ah_work_run(f->engine, session, &error), &error);
    assert(state.parent_calls == 1 && state.leaf_calls == 1);
    ah_close(f->engine);
}

static ah_code invalid_children(void *context, const ah_tool_call *call, ah_tool_result *result,
                                ah_error *error) {
    (void)call;
    (void)error;
    nested_fixture *f = context;
    f->parent_calls++;
    result->continuation_json = strdup("{\"checkpoint\":{\"phase\":99},\"calls\":["
                                       "{\"key\":\"first\",\"tool\":\"leaf\",\"arguments\":{}},"
                                       "{\"key\":\"second\",\"tool\":\"absent\",\"arguments\":{}}],"
                                       "\"wait\":{\"calls\":[\"first\"],\"policy\":\"all\"}}");
    assert(result->continuation_json);
    return AH_OK;
}

static void invalid_continuation_is_atomic(void) {
    nested_fixture state = {0};
    durable_fixture *f = &state.fixture;
    df_open(f, ":memory:", 1);
    ah_work_config work = {.version = 1, .replay = AH_REPLAY_SAFE};
    ah_tool tool = {.name = "durable",
                    .description = "Invalid second child",
                    .parameters_json = "{\"type\":\"object\"}",
                    .effect = AH_EFFECT_READ,
                    .execute = invalid_children,
                    .context = &state,
                    .work = &work};
    ah_error error = {0};
    df_ok(ah_register_tool(f->engine, &tool, &error), &error);
    tool.name = "leaf";
    tool.execute = leaf;
    tool.work = NULL;
    df_ok(ah_register_tool(f->engine, &tool, &error), &error);
    char session[AH_ID_CAP], operation[AH_ID_CAP];
    df_ok(ah_session_create(f->engine, "mock", "", session, &error), &error);
    df_ok(ah_work_start(f->engine, session, "root", "durable", "{}", operation, &error), &error);
    df_ok(ah_work_run(f->engine, session, &error), &error);
    assert(state.parent_calls == 1 && state.leaf_calls == 0);
    yyjson_doc *doc = nested_snapshot(f, session, operation);
    yyjson_val *root = yyjson_doc_get_root(doc);
    assert(yyjson_equals_str(yyjson_obj_get(root, "status"), "done"));
    assert(yyjson_get_bool(yyjson_obj_get(root, "is_error")));
    assert(yyjson_arr_size(yyjson_obj_get(root, "children")) == 0);
    assert(!yyjson_obj_get(yyjson_obj_get(root, "checkpoint"), "phase"));
    yyjson_doc_free(doc);
    ah_close(f->engine);
}

static void cancel_parent_with_unknown_child(void) {
    nested_fixture state = {.unknown_leaf = true};
    durable_fixture *f = &state.fixture;
    df_open(f, ":memory:", 1);
    register_nested(&state, false);
    ah_error error = {0};
    char session[AH_ID_CAP], operation[AH_ID_CAP], child_id[AH_ID_CAP];
    df_ok(ah_session_create(f->engine, "mock", "", session, &error), &error);
    df_ok(ah_work_start(f->engine, session, "parent", "durable", "{}", operation, &error), &error);
    assert(ah_work_run(f->engine, session, &error) == AH_UNCERTAIN);
    assert(state.parent_calls == 1 && state.leaf_calls == 1);
    yyjson_doc *doc = nested_snapshot(f, session, operation);
    yyjson_val *children = yyjson_obj_get(yyjson_doc_get_root(doc), "children");
    assert(yyjson_arr_size(children) == 1);
    yyjson_val *child = yyjson_arr_get(children, 0);
    assert(yyjson_equals_str(yyjson_obj_get(child, "status"), "unknown"));
    const char *id = yyjson_get_str(yyjson_obj_get(child, "operation_id"));
    assert(id);
    snprintf(child_id, sizeof(child_id), "%s", id);
    yyjson_doc_free(doc);
    assert(ah_work_cancel(f->engine, session, operation, &error) == AH_UNCERTAIN);
    df_ok(ah_resolve(f->engine, session, child_id, "{\"reconciled\":true}", false, &error), &error);
    df_ok(ah_work_run(f->engine, session, &error), &error);
    assert(state.parent_calls == 1 && state.leaf_calls == 1);
    doc = nested_snapshot(f, session, operation);
    yyjson_val *root = yyjson_doc_get_root(doc);
    assert(yyjson_equals_str(yyjson_obj_get(root, "status"), "done"));
    assert(yyjson_get_bool(yyjson_obj_get(root, "is_error")));
    yyjson_doc_free(doc);
    ah_close(f->engine);
}

static ah_code parked_sibling(void *context, const ah_tool_call *call, ah_tool_result *result,
                              ah_error *error) {
    (void)context;
    (void)error;
    if (!strcmp(call->checkpoint_json, "{}"))
        result->continuation_json =
            strdup("{\"checkpoint\":{\"phase\":1},\"wait\":{\"signal\":\"ready\"}}");
    else {
        assert(call->wake_json);
        result->json = strdup("{\"ready\":true}");
    }
    assert(result->json || result->continuation_json);
    return AH_OK;
}

static void later_root_subtree_progresses(void) {
    nested_fixture state = {0};
    durable_fixture *f = &state.fixture;
    df_open(f, ":memory:", 2);
    ah_work_config work = {.version = 1, .replay = AH_REPLAY_SAFE};
    ah_tool tool = {.name = "waiter",
                    .description = "Park an earlier independent root",
                    .parameters_json = "{\"type\":\"object\"}",
                    .effect = AH_EFFECT_READ,
                    .execute = parked_sibling,
                    .context = &state,
                    .work = &work};
    ah_error error = {0};
    df_ok(ah_register_background_tool(f->engine, &tool, &error), &error);
    tool.name = "durable";
    tool.execute = parent;
    df_ok(ah_register_background_tool(f->engine, &tool, &error), &error);
    tool.name = "leaf";
    tool.execute = leaf;
    tool.work = NULL;
    df_ok(ah_register_background_tool(f->engine, &tool, &error), &error);
    char session[AH_ID_CAP], waiter[AH_ID_CAP], operation[AH_ID_CAP];
    df_ok(ah_session_create(f->engine, "mock", "", session, &error), &error);
    df_ok(ah_work_start(f->engine, session, "first", "waiter", "{}", waiter, &error), &error);
    df_ok(ah_work_start(f->engine, session, "second", "durable", "{}", operation, &error), &error);
    df_ok(ah_work_run(f->engine, session, &error), &error);
    df_state(f, session, AH_WAITING_WORK);
    assert(state.parent_calls == 3 && state.leaf_calls == 1);
    yyjson_doc *doc = nested_snapshot(f, session, operation);
    assert(yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(doc), "status"), "done"));
    yyjson_doc_free(doc);
    ah_signal_result delivered;
    df_ok(ah_signal(f->engine, session, waiter, "ready", "true", &delivered, &error), &error);
    assert(delivered == AH_SIGNAL_DELIVERED);
    df_ok(ah_work_run(f->engine, session, &error), &error);
    df_state(f, session, AH_COMPLETED);
    assert(f->models == 0 && state.parent_calls == 3 && state.leaf_calls == 1);
    ah_close(f->engine);
}

typedef struct {
    durable_fixture fixture;
    atomic_uint parent_calls, failed_calls, active_calls;
    atomic_bool active_entered, active_drained;
} fail_fast_fixture;

static ah_code fail_fast_child(void *context, const ah_tool_call *call, ah_tool_result *result,
                               ah_error *error) {
    (void)error;
    fail_fast_fixture *f = context;
    uint64_t deadline = ah_now_ms() + 2000;
    if (!strcmp(call->arguments_json, "{\"fail\":true}")) {
        atomic_fetch_add(&f->failed_calls, 1);
        while (!atomic_load(&f->active_entered) && ah_now_ms() < deadline)
            df_sleep(1);
        assert(atomic_load(&f->active_entered));
        result->json = strdup("{\"error\":\"planned child failure\"}");
    } else {
        atomic_fetch_add(&f->active_calls, 1);
        atomic_store(&f->active_entered, true);
        while (!call->cancelled(call->cancel_context) && ah_now_ms() < deadline)
            df_sleep(1);
        assert(call->cancelled(call->cancel_context));
        /* The parent must not enter while cancellation cleanup is still active. */
        df_sleep(20);
        atomic_store(&f->active_drained, true);
        result->json = strdup("{\"cancelled\":true}");
    }
    result->is_error = true;
    assert(result->json);
    return AH_OK;
}

static ah_code fail_fast_parent(void *context, const ah_tool_call *call, ah_tool_result *result,
                                ah_error *error) {
    (void)error;
    fail_fast_fixture *f = context;
    unsigned invocation = atomic_fetch_add(&f->parent_calls, 1);
    if (!invocation) {
        result->continuation_json =
            strdup("{\"checkpoint\":{\"phase\":1},\"calls\":["
                   "{\"key\":\"failed\",\"tool\":\"child\",\"arguments\":{\"fail\":true}},"
                   "{\"key\":\"active\",\"tool\":\"child\",\"arguments\":{}}],"
                   "\"wait\":{\"calls\":[\"failed\",\"active\"],\"policy\":\"fail_fast\"}}");
        assert(result->continuation_json);
        return AH_OK;
    }
    assert(invocation == 1 && atomic_load(&f->active_drained));
    assert(atomic_load(&f->failed_calls) == 1 && atomic_load(&f->active_calls) == 1);
    assert(call->wake_json);
    yyjson_doc *wake = yyjson_read(call->wake_json, strlen(call->wake_json), 0);
    assert(wake);
    yyjson_val *root = yyjson_doc_get_root(wake);
    assert(yyjson_equals_str(yyjson_obj_get(root, "reason"), "calls"));
    yyjson_val *calls = yyjson_obj_get(root, "calls");
    yyjson_val *failed = yyjson_obj_get(calls, "failed"), *active = yyjson_obj_get(calls, "active");
    assert(yyjson_equals_str(yyjson_obj_get(failed, "status"), "done"));
    assert(yyjson_get_bool(yyjson_obj_get(failed, "is_error")));
    assert(yyjson_equals_str(yyjson_obj_get(yyjson_obj_get(failed, "result"), "error"),
                             "planned child failure"));
    assert(yyjson_equals_str(yyjson_obj_get(active, "status"), "done"));
    assert(yyjson_get_bool(yyjson_obj_get(active, "is_error")));
    yyjson_doc_free(wake);
    result->json = strdup("{\"handled\":true}");
    assert(result->json);
    return AH_OK;
}

static void fail_fast_drains_active_child(void) {
    fail_fast_fixture state = {0};
    durable_fixture *f = &state.fixture;
    df_open(f, ":memory:", 2);
    ah_work_config work = {.version = 1, .replay = AH_REPLAY_SAFE};
    ah_tool tool = {.name = "durable",
                    .description = "Handle a failed child after draining its sibling",
                    .parameters_json = "{\"type\":\"object\"}",
                    .effect = AH_EFFECT_READ,
                    .execute = fail_fast_parent,
                    .context = &state,
                    .work = &work};
    ah_error error = {0};
    df_ok(ah_register_background_tool(f->engine, &tool, &error), &error);
    tool.name = "child";
    tool.execute = fail_fast_child;
    tool.work = NULL;
    df_ok(ah_register_background_tool(f->engine, &tool, &error), &error);
    char session[AH_ID_CAP], operation[AH_ID_CAP];
    df_ok(ah_session_create(f->engine, "mock", "", session, &error), &error);
    df_ok(ah_work_start(f->engine, session, "root", "durable", "{}", operation, &error), &error);
    df_ok(ah_work_run(f->engine, session, &error), &error);
    df_state(f, session, AH_COMPLETED);
    yyjson_doc *doc = nested_snapshot(f, session, operation);
    yyjson_val *root = yyjson_doc_get_root(doc);
    assert(yyjson_equals_str(yyjson_obj_get(root, "status"), "done"));
    assert(yyjson_is_false(yyjson_obj_get(root, "is_error")));
    assert(yyjson_get_bool(yyjson_obj_get(yyjson_obj_get(root, "result"), "handled")));
    assert(yyjson_arr_size(yyjson_obj_get(root, "children")) == 2);
    yyjson_doc_free(doc);
    df_ok(ah_work_run(f->engine, session, &error), &error);
    assert(atomic_load(&state.parent_calls) == 2 && atomic_load(&state.failed_calls) == 1 &&
           atomic_load(&state.active_calls) == 1 && f->models == 0);
    ah_close(f->engine);
}

void df_composition_cases(void) {
    nested_key_reuse(false, false);
    nested_key_reuse(true, false);
    nested_key_reuse(false, true);
    cancel_parent_and_child();
    invalid_continuation_is_atomic();
    cancel_parent_with_unknown_child();
    later_root_subtree_progresses();
    fail_fast_drains_active_child();
}
