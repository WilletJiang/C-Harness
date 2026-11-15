#include "durable_fixture.h"
#include "ah/work.h"
#include "ah/jobs.h"

typedef struct {
    durable_fixture fixture;
    bool crash, park, same_checkpoint, migrate;
    unsigned observed_phase, migrations;
    uint64_t observed_generation;
} recovery_fixture;

static ah_code execute(void *context, const ah_tool_call *call, ah_tool_result *result,
                       ah_error *error) {
    (void)error;
    recovery_fixture *state = context;
    unsigned invocation = atomic_fetch_add(&state->fixture.calls, 1) + 1;
    yyjson_doc *doc = yyjson_read(call->checkpoint_json, strlen(call->checkpoint_json), 0);
    assert(doc);
    state->observed_phase =
        (unsigned)yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(doc), "phase"));
    state->observed_generation = call->generation;
    yyjson_doc_free(doc);
    if (state->crash && state->same_checkpoint && invocation == 1) {
        result->continuation_json = strdup("{\"checkpoint\":{}}");
        assert(result->continuation_json);
        return AH_OK;
    }
    if (state->crash)
        df_ready(state->fixture.ready_fd);
    if (state->park && !state->observed_phase)
        result->continuation_json =
            strdup("{\"checkpoint\":{\"phase\":1},\"wait\":{\"signal\":\"ready\"}}");
    else
        result->json = strdup("{\"recovered\":true}");
    assert(result->json || result->continuation_json);
    return AH_OK;
}

static ah_code migrate_checkpoint(void *context, unsigned version, const char *arguments,
                                  const char *checkpoint, char **out, ah_error *error) {
    (void)error;
    recovery_fixture *state = context;
    assert(pthread_equal(state->fixture.owner, pthread_self()));
    assert(version == 1 && !strcmp(arguments, "{}"));
    yyjson_doc *doc = yyjson_read(checkpoint, strlen(checkpoint), 0);
    assert(doc);
    assert(yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(doc), "phase")) == 1);
    yyjson_doc_free(doc);
    state->migrations++;
    *out = strdup("{\"phase\":2}");
    assert(*out);
    return AH_OK;
}

static void register_recovery(recovery_fixture *state, const char *mode, unsigned version) {
    ah_work_config work = {.version = version,
                           .replay = strstr(mode, "unsafe") ? AH_REPLAY_NEVER : AH_REPLAY_SAFE,
                           .max_restarts = 2,
                           .migrate = state->migrate ? migrate_checkpoint : NULL};
    ah_tool tool = {.name = "durable",
                    .description = "Killed durable step",
                    .parameters_json = "{\"type\":\"object\"}",
                    .effect = strstr(mode, "external") ? AH_EFFECT_EXTERNAL : AH_EFFECT_READ,
                    .execute = execute,
                    .context = state,
                    .work = &work};
    ah_error error = {0};
    df_ok(ah_register_tool(state->fixture.engine, &tool, &error), &error);
}

static void resume_if_stopped(durable_fixture *f, const char *session) {
    ah_error error = {0};
    ah_state state;
    df_ok(ah_session_state(f->engine, session, &state, &error), &error);
    if (state == AH_STOPPED)
        df_ok(ah_resume(f->engine, session, &error), &error);
}

static yyjson_doc *snapshot(durable_fixture *f, const char *session, const char *operation) {
    ah_error error = {0};
    char *json = NULL;
    df_ok(ah_work_json(f->engine, session, operation, &json, &error), &error);
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    assert(doc);
    ah_free(json);
    return doc;
}

static void create(recovery_fixture *state, const char *database, const char *mode,
                   char session[AH_ID_CAP], char operation[AH_ID_CAP]) {
    durable_fixture *f = &state->fixture;
    df_open(f, database, 1);
    register_recovery(state, mode, 1);
    ah_error error = {0};
    df_ok(ah_session_create(f->engine, "mock", "", session, &error), &error);
    df_ok(ah_work_start(f->engine, session, "recover", "durable", "{}", operation, &error), &error);
    ah_close(f->engine);
}

static ah_code ordered_read(void *context, const ah_tool_call *call, ah_tool_result *result,
                            ah_error *error) {
    (void)error;
    durable_fixture *f = context;
    atomic_fetch_add(&f->calls, 1);
    if (f->crash_after_receipt && !strcmp(call->operation_id, f->operations[0])) {
        while (!call->cancelled(call->cancel_context))
            df_sleep(1);
        abort();
    }
    result->json = strdup("{\"value\":7}");
    assert(result->json);
    return AH_OK;
}

static void register_ordered(durable_fixture *f) {
    ah_work_config work = {.version = 1, .replay = AH_REPLAY_SAFE};
    ah_tool tool = {.name = "durable",
                    .description = "Recover first read before publishing second",
                    .parameters_json = "{\"type\":\"object\"}",
                    .effect = AH_EFFECT_READ,
                    .execute = ordered_read,
                    .context = f,
                    .work = &work};
    ah_error error = {0};
    df_ok(ah_register_background_tool(f->engine, &tool, &error), &error);
}

int df_recovery_child(int argc, char **argv) {
    if (argc != 5 || strncmp(argv[1], "--durable-", 10))
        return -1;
    if (!strcmp(argv[1], "--durable-order")) {
        durable_fixture f = {.batch = 2, .crash_after_receipt = true, .ready_fd = atoi(argv[4])};
        df_open(&f, argv[2], 2);
        register_ordered(&f);
        ah_error error = {0};
        df_ok(ah_run(f.engine, argv[3], &error), &error);
        return 88;
    }
    recovery_fixture state = {.crash = !strstr(argv[1], "park"),
                              .park = strstr(argv[1], "park") != NULL,
                              .same_checkpoint = strstr(argv[1], "same") != NULL};
    state.fixture.ready_fd = atoi(argv[4]);
    df_open(&state.fixture, argv[2], 1);
    register_recovery(&state, argv[1], 1);
    resume_if_stopped(&state.fixture, argv[3]);
    ah_error error = {0};
    df_ok(ah_work_run(state.fixture.engine, argv[3], &error), &error);
    assert(state.park && atomic_load(&state.fixture.calls) == 1);
    df_ready(state.fixture.ready_fd);
    return 88;
}

static void parked_recovery(const char *executable, unsigned version_mode) {
    char database[64], session[AH_ID_CAP], operation[AH_ID_CAP];
    df_database(database);
    recovery_fixture state = {.migrate = version_mode == 2};
    durable_fixture *f = &state.fixture;
    create(&state, database, "--durable-park", session, operation);
    df_kill_child(executable, "--durable-park", database, session);
    df_open(f, database, 1);
    register_recovery(&state, "--durable-park", version_mode ? 2 : 1);
    resume_if_stopped(f, session);
    ah_error error = {0};
    ah_signal_result delivered;
    df_ok(ah_signal(f->engine, session, operation, "ready", "true", &delivered, &error), &error);
    assert(delivered == AH_SIGNAL_DELIVERED);
    df_ok(ah_work_run(f->engine, session, &error), &error);
    if (version_mode == 1) {
        assert(atomic_load(&f->calls) == 0);
        df_state(f, session, AH_WAITING_WORK);
        ah_close(f->engine);
        df_open(f, database, 1);
        register_recovery(&state, "--durable-park", 1);
        resume_if_stopped(f, session);
        df_ok(ah_work_run(f->engine, session, &error), &error);
    }
    assert(atomic_load(&f->calls) == 1 && state.observed_phase == (version_mode == 2 ? 2 : 1));
    assert(state.migrations == (version_mode == 2 ? 1 : 0));
    assert(state.observed_generation >= 2 && f->models == 0);
    yyjson_doc *doc = snapshot(f, session, operation);
    assert(yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(doc), "status"), "done"));
    yyjson_doc_free(doc);
    df_ok(ah_work_run(f->engine, session, &error), &error);
    assert(atomic_load(&f->calls) == 1);
    ah_close(f->engine);
    df_remove_database(database);
}

static void executing_recovery(const char *executable, const char *mode) {
    char database[64], session[AH_ID_CAP], operation[AH_ID_CAP];
    df_database(database);
    recovery_fixture state = {0};
    durable_fixture *f = &state.fixture;
    create(&state, database, mode, session, operation);
    df_kill_child(executable, mode, database, session);
    df_open(f, database, 1);
    register_recovery(&state, mode, 1);
    ah_error error = {0};
    if (strstr(mode, "external")) {
        df_state(f, session, AH_OUTCOME_UNKNOWN);
        assert(ah_work_run(f->engine, session, &error) == AH_UNCERTAIN);
        df_ok(ah_resolve(f->engine, session, operation, "{\"reconciled\":true}", false, &error),
              &error);
    }
    resume_if_stopped(f, session);
    df_ok(ah_work_run(f->engine, session, &error), &error);
    unsigned expected = strstr(mode, "unsafe") ? 0 : 1;
    assert(atomic_load(&f->calls) == expected && f->models == 0);
    if (expected)
        assert(state.observed_phase == 0 && state.observed_generation >= 2);
    yyjson_doc *doc = snapshot(f, session, operation);
    yyjson_val *root = yyjson_doc_get_root(doc);
    assert(yyjson_equals_str(yyjson_obj_get(root, "status"), "done"));
    if (strstr(mode, "unsafe") && !strstr(mode, "external"))
        assert(yyjson_get_bool(yyjson_obj_get(root, "is_error")));
    yyjson_doc_free(doc);
    ah_close(f->engine);
    df_remove_database(database);
}

static void revoked_replay(const char *executable, bool external) {
    char database[64], session[AH_ID_CAP], operation[AH_ID_CAP];
    df_database(database);
    recovery_fixture state = {0};
    durable_fixture *f = &state.fixture;
    const char *original = external ? "--durable-safe-external" : "--durable-safe";
    create(&state, database, original, session, operation);
    df_kill_child(executable, original, database, session);
    df_open(f, database, 1);
    register_recovery(&state, external ? "--durable-unsafe-external" : "--durable-unsafe", 1);
    resume_if_stopped(f, session);
    ah_error error = {0};
    ah_code code = ah_work_run(f->engine, session, &error);
    if (external) {
        assert(code == AH_UNCERTAIN);
        df_ok(ah_resolve(f->engine, session, operation, "{}", false, &error), &error);
        df_ok(ah_work_run(f->engine, session, &error), &error);
    } else
        df_ok(code, &error);
    assert(atomic_load(&f->calls) == 0);
    yyjson_doc *doc = snapshot(f, session, operation);
    yyjson_val *root = yyjson_doc_get_root(doc);
    assert(yyjson_equals_str(yyjson_obj_get(root, "status"), "done"));
    assert(yyjson_get_bool(yyjson_obj_get(root, "is_error")) == !external);
    yyjson_doc_free(doc);
    ah_close(f->engine);
    df_remove_database(database);
}

static void bounded_restarts(const char *executable, const char *mode) {
    char database[64], session[AH_ID_CAP], operation[AH_ID_CAP];
    df_database(database);
    recovery_fixture state = {0};
    durable_fixture *f = &state.fixture;
    create(&state, database, mode, session, operation);
    unsigned attempts = 0;
    bool settled = false;
    while (!settled && attempts < 4) {
        df_kill_child(executable, mode, database, session);
        attempts++;
        df_open(f, database, 1);
        register_recovery(&state, mode, 1);
        yyjson_doc *doc = snapshot(f, session, operation);
        yyjson_val *root = yyjson_doc_get_root(doc);
        settled = yyjson_equals_str(yyjson_obj_get(root, "status"), "done");
        if (settled)
            assert(yyjson_get_bool(yyjson_obj_get(root, "is_error")));
        yyjson_doc_free(doc);
        ah_close(f->engine);
    }
    assert(settled && attempts >= 2 && attempts <= 3 && atomic_load(&f->calls) == 0);
    df_remove_database(database);
}

static void recovered_receipt_order(const char *executable) {
    char database[64], session[AH_ID_CAP];
    df_database(database);
    durable_fixture f = {.batch = 2};
    df_open(&f, database, 2);
    register_ordered(&f);
    df_create(&f, session);
    ah_close(f.engine);
    df_kill_child(executable, "--durable-order", database, session);
    df_open(&f, database, 2);
    register_ordered(&f);
    resume_if_stopped(&f, session);
    ah_error error = {0};
    df_ok(ah_run(f.engine, session, &error), &error);
    df_state(&f, session, AH_COMPLETED);
    assert(atomic_load(&f.calls) == 1 && f.models == 1 && f.receipts == 2);
    assert(df_events(&f, session, "tool.completed") == 2);
    ah_close(f.engine);
    df_remove_database(database);
}

void df_recovery_cases(const char *executable) {
    parked_recovery(executable, 0);
    parked_recovery(executable, 1);
    parked_recovery(executable, 2);
    executing_recovery(executable, "--durable-safe");
    executing_recovery(executable, "--durable-unsafe");
    executing_recovery(executable, "--durable-unsafe-external");
    revoked_replay(executable, false);
    revoked_replay(executable, true);
    bounded_restarts(executable, "--durable-safe");
    bounded_restarts(executable, "--durable-same");
    recovered_receipt_order(executable);
}
