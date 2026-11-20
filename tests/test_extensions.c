#include "agent_fixture.h"
#include "extensions/extensions_internal.h"
#include <sys/stat.h>
#include <unistd.h>

static const char old_body[] = "SKILL_PRIVATE_BODY_ORIGINAL_741";
static const char new_body[] = "SKILL_PRIVATE_BODY_RELOADED_852";
static const char secret[] = "CATALOG_PRIVATE_CREDENTIAL_963";

void extension_reference_cases(ah_engine *, const char *, const char *, const char *);

typedef struct {
    agent_fixture agent;
    char home[128], database[256], primary[256], secondary[256];
    char skill[96], other[96], revision[65];
    char *catalog, *previous_request;
} fixture;

static void path(char *out, size_t capacity, const char *root, const char *suffix) {
    int length = snprintf(out, capacity, "%s/%s", root, suffix);
    assert(length > 0 && (size_t)length < capacity);
}

static void write_bytes(const char *filename, const char *text, size_t size) {
    FILE *file = fopen(filename, "wb");
    assert(file && fwrite(text, 1, size, file) == size);
    assert(fclose(file) == 0);
}

static void write_text(const char *filename, const char *text) {
    write_bytes(filename, text, strlen(text));
}

static void write_skill(const char *filename, const char *name, const char *body) {
    FILE *file = fopen(filename, "wb");
    assert(file);
    assert(fprintf(file, "---\nname: %s\ndescription: Extension fixture 741852963\n---\n%s\n", name,
                   body) > 0);
    assert(fclose(file) == 0);
}

static char *snapshot(ah_engine *engine, const char *session) {
    ah_error error = {0};
    core_session state;
    af_ok(core_load(engine, session, &state, &error), &error);
    char *request = NULL;
    af_ok(core_request(engine, &state, &request, &error), &error);
    core_session_free(&state);
    return request;
}

static void body_count(ah_engine *engine, const char *session, const char *body, unsigned count) {
    char *request = snapshot(engine, session);
    assert(af_occurrences(request, body) == count);
    free(request);
}

static char *skill_arguments(const char *id, const char *action) {
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "id", id);
    yyjson_mut_obj_add_str(doc, root, "action", action);
    char *json = ah_mut_write(doc);
    ah_mut_doc_free(doc);
    assert(json);
    return json;
}

static ah_code skill_action(fixture *f, const char *session, const char *id, const char *action) {
    char *arguments = skill_arguments(id, action);
    ah_tool_call call = {.session_id = session, .arguments_json = arguments};
    ah_tool_result result = {0};
    ah_error error = {0};
    ah_code code = ext_skill(f->agent.engine->extensions, &call, &result, &error);
    if (code == AH_IO)
        fprintf(stderr, "skill %s failed: %s\n", action, error.message);
    if (code == AH_OK)
        assert(result.json && !strstr(result.json, old_body) && !strstr(result.json, new_body));
    free(result.json);
    free(arguments);
    return code;
}

static char *skill_reply(const char *id, const char *call) {
    char *arguments = skill_arguments(id, "load");
    char *response = af_call("skill", call, arguments);
    free(arguments);
    return response;
}

static char *plain_reply(agent_fixture *agent, agent_request *request) {
    (void)agent;
    (void)request;
    return af_text("done");
}

static char *runtime_reply(agent_fixture *agent, agent_request *request) {
    fixture *f = (fixture *)agent;
    assert(!strstr(request->request, secret));
    if (strcmp(request->session, agent->root)) {
        assert(!strstr(request->request, old_body));
        assert(!strstr(request->request, new_body));
        return af_text("fresh child completed");
    }
    unsigned phase = agent->phase++;
    if (f->previous_request) {
        yyjson_doc *before = yyjson_read(f->previous_request, strlen(f->previous_request), 0),
                   *after = yyjson_read(request->request, strlen(request->request), 0);
        assert(before && after);
        yyjson_val *old_input = yyjson_obj_get(yyjson_doc_get_root(before), "input"),
                   *new_input = yyjson_obj_get(yyjson_doc_get_root(after), "input");
        assert(yyjson_arr_size(new_input) >= yyjson_arr_size(old_input));
        for (size_t i = 0; i < yyjson_arr_size(old_input); ++i)
            assert(yyjson_equals(yyjson_arr_get(old_input, i), yyjson_arr_get(new_input, i)));
        yyjson_doc_free(before);
        yyjson_doc_free(after);
    }
    free(f->previous_request);
    f->previous_request = ah_strdup(request->request);
    assert(f->previous_request);
    assert(af_occurrences(request->request, old_body) == (phase >= 3 ? 1 : 0));
    if (!phase)
        return af_call("discover", "catalog", "{\"kind\":\"all\",\"query\":\"741852963\"}");
    if (phase == 1) {
        yyjson_doc *receipt = af_receipt(request->request, "catalog");
        yyjson_val *result = af_result(receipt), *entry;
        f->catalog = yyjson_val_write(result, 0, NULL);
        assert(f->catalog && !strstr(f->catalog, old_body) && !strstr(f->catalog, secret));
        size_t index, count;
        yyjson_arr_foreach(yyjson_obj_get(result, "items"), index, count, entry) {
            const char *id = yyjson_get_str(yyjson_obj_get(entry, "id"));
            const char *name = yyjson_get_str(yyjson_obj_get(entry, "name"));
            assert(id && name);
            if (!strcmp(name, "primary")) {
                assert(strlen(id) < sizeof(f->skill));
                strcpy(f->skill, id);
            } else if (!strcmp(name, "secondary")) {
                assert(strlen(id) < sizeof(f->other));
                strcpy(f->other, id);
            }
        }
        assert(*f->skill && *f->other);
        yyjson_doc_free(receipt);
        sqlite3_stmt *row = NULL;
        assert(
            sqlite3_prepare_v2(agent->engine->db,
                               "SELECT id FROM operations WHERE session_id=? AND call_id='catalog'",
                               -1, &row, NULL) == SQLITE_OK);
        sqlite3_bind_text(row, 1, agent->root, -1, SQLITE_TRANSIENT);
        assert(sqlite3_step(row) == SQLITE_ROW);
        char arguments[128];
        snprintf(arguments, sizeof(arguments), "{\"operation_id\":\"%s\",\"limit\":4096}",
                 sqlite3_column_text(row, 0));
        sqlite3_finalize(row);
        return af_call("read_result", "catalog_saved", arguments);
    }
    if (phase == 2) {
        yyjson_doc *receipt = af_receipt(request->request, "catalog_saved");
        yyjson_val *result = af_result(receipt);
        assert(!strcmp(yyjson_get_str(yyjson_obj_get(result, "content")), f->catalog));
        assert(yyjson_is_false(yyjson_obj_get(result, "truncated")));
        yyjson_doc_free(receipt);
        return skill_reply(f->skill, "load_once");
    }
    if (phase == 3 || phase == 4) {
        yyjson_doc *receipt = af_receipt(request->request, phase == 3 ? "load_once" : "load_twice");
        yyjson_val *result = af_result(receipt);
        const char *revision = yyjson_get_str(yyjson_obj_get(result, "revision"));
        assert(revision && strlen(revision) == 64);
        if (phase == 3)
            strcpy(f->revision, revision);
        else
            assert(!strcmp(f->revision, revision));
        yyjson_doc_free(receipt);
        if (phase == 3)
            return skill_reply(f->skill, "load_twice");
        return af_call("spawn", "fresh", "{\"name\":\"fresh\",\"message\":\"Independent task\"}");
    }
    if (phase == 5) {
        af_spawn_id(request->request, "fresh", agent->ids[0]);
        const char *ids[] = {agent->ids[0]};
        return af_join("joined", ids, 1, 1000);
    }
    assert(phase == 6);
    return af_text("extension flow completed");
}

static void enable(fixture *f, size_t context_bytes) {
    ah_extensions_config config;
    ah_extensions_config_init(&config);
    config.codex_home = f->home;
    config.enable_skills = config.enable_mcp = true;
    config.max_context_bytes = context_bytes;
    ah_error error = {0};
    af_ok(ah_extensions_enable(f->agent.engine, &config, &error), &error);
}

static void disabled_is_inert(void) {
    agent_fixture agent = {.reply = plain_reply};
    af_open(&agent, 1, 4, 2);
    ah_extensions_config config;
    ah_extensions_config_init(&config);
    assert(!config.enable_skills && !config.enable_mcp && !config.enable_plugins);
    assert(config.approve_mcp_calls);
    config.codex_home = "/missing-extension-home";
    config.workspace = "/missing-extension-workspace";
    config.max_context_bytes = 0; /* Disabled features do not validate or read their sources. */
    ah_error error = {0};
    size_t before = agent.engine->tool_count;
    af_ok(ah_extensions_enable(agent.engine, &config, &error), &error);
    assert(!agent.engine->extensions && agent.engine->tool_count == before);
    af_create(&agent, "default invocation");
    af_run(&agent);
    assert(!strstr(agent.requests[0].request, "discover"));
    af_close(&agent);
}

static void lifecycle(fixture *f) {
    agent_fixture *agent = &f->agent;
    ah_error error = {0};
    body_count(agent->engine, agent->root, old_body, 1);
    body_count(agent->engine, agent->ids[0], old_body, 0);
    extension_reference_cases(agent->engine, agent->root, f->skill, f->primary);
    write_skill(f->primary, "primary", new_body);
    body_count(agent->engine, agent->root, old_body,
               1); /* Disk edits require an explicit reload. */
    assert(skill_action(f, agent->root, f->skill, "load") == AH_OK);
    body_count(agent->engine, agent->root, old_body, 0);
    body_count(agent->engine, agent->root, new_body, 1);
    sqlite3_stmt *row = NULL;
    assert(sqlite3_prepare_v2(agent->engine->db,
                              "SELECT revision FROM extension_context WHERE session_id=?", -1, &row,
                              NULL) == SQLITE_OK);
    sqlite3_bind_text(row, 1, agent->root, -1, SQLITE_TRANSIENT);
    assert(sqlite3_step(row) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(row, 0), f->revision));
    assert(sqlite3_step(row) == SQLITE_DONE);
    sqlite3_finalize(row);

    assert(skill_action(f, agent->root, f->other, "load") == AH_LIMIT);
    body_count(agent->engine, agent->root, new_body, 1);
    body_count(agent->engine, agent->root, "SECOND_SKILL_BODY_174", 0);
    char oversized[2048];
    memset(oversized, 'x', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = 0;
    write_skill(f->primary, "primary", oversized);
    assert(skill_action(f, agent->root, f->skill, "load") == AH_LIMIT);
    body_count(agent->engine, agent->root, new_body, 1);
    write_text(f->primary, "invalid UTF-8: \xff");
    assert(skill_action(f, agent->root, f->skill, "load") == AH_INVALID);
    body_count(agent->engine, agent->root, new_body, 1);
    write_bytes(f->primary, "embedded\0NUL", 12);
    assert(skill_action(f, agent->root, f->skill, "load") == AH_INVALID);
    body_count(agent->engine, agent->root, new_body, 1);
    write_skill(f->primary, "primary", new_body);

    af_ok(ah_compact(agent->engine, agent->root, "Completed extension work", 0, &error), &error);
    body_count(agent->engine, agent->root, new_body, 1);
    char forked[AH_ID_CAP];
    af_ok(ah_fork(agent->engine, agent->root, forked, &error), &error);
    body_count(agent->engine, forked, new_body, 1);
    assert(skill_action(f, forked, f->skill, "unload") == AH_OK);
    body_count(agent->engine, forked, new_body, 0);
    body_count(agent->engine, agent->root, new_body, 1);

    af_reopen(agent);
    body_count(agent->engine, agent->root, new_body, 0);
    assert(!core_find_tool(agent->engine, "skill"));
    enable(f, 1024);
    body_count(agent->engine, agent->root, new_body, 1);
    assert(skill_action(f, agent->root, f->skill, "unload") == AH_OK);
    assert(skill_action(f, agent->root, f->skill, "unload") == AH_OK);
    body_count(agent->engine, agent->root, new_body, 0);
    assert(skill_action(f, agent->root, f->other, "load") == AH_OK);
    body_count(agent->engine, agent->root, "SECOND_SKILL_BODY_174", 1);
    af_reopen(agent);
    enable(f, 4096);
    assert(skill_action(f, agent->root, f->skill, "load") == AH_OK);
    af_reopen(agent);
    enable(f, 1024);
    core_session state;
    af_ok(core_load(agent->engine, agent->root, &state, &error), &error);
    char *request = NULL;
    assert(core_request(agent->engine, &state, &request, &error) == AH_LIMIT);
    assert(!request);
    core_session_free(&state);
    assert(skill_action(f, agent->root, f->skill, "unload") == AH_OK);
    body_count(agent->engine, agent->root, "SECOND_SKILL_BODY_174", 1);
}

static void runtime_and_context(bool utf16) {
    fixture f = {.agent = {.reply = runtime_reply}};
    strcpy(f.home, "/tmp/ah-extension-context-XXXXXX");
    assert(mkdtemp(f.home));
    char filename[256];
    const char *directories[] = {"skills", "skills/primary", "skills/secondary"};
    for (size_t i = 0; i < 3; ++i) {
        path(filename, sizeof(filename), f.home, directories[i]);
        assert(mkdir(filename, 0700) == 0);
    }
    path(f.primary, sizeof(f.primary), f.home, "skills/primary/SKILL.md");
    path(f.secondary, sizeof(f.secondary), f.home, "skills/secondary/SKILL.md");
    write_skill(f.primary, "primary", old_body);
    write_skill(f.secondary, "secondary", "SECOND_SKILL_BODY_174");
    path(filename, sizeof(filename), f.home, "config.toml");
    write_text(filename,
               "[mcp_servers.fixture_741852963]\ncommand='not-an-executable'\n"
               "args=['CATALOG_PRIVATE_CREDENTIAL_963']\n"
               "[mcp_servers.fixture_741852963.env]\nTOKEN='CATALOG_PRIVATE_CREDENTIAL_963'\n");
    path(f.database, sizeof(f.database), f.home, "state.db");
    if (utf16) {
        sqlite3 *database = NULL;
        assert(sqlite3_open(f.database, &database) == SQLITE_OK);
        assert(sqlite3_exec(database, "PRAGMA encoding='UTF-16'; CREATE TABLE encoding_fixture(x)",
                            NULL, NULL, NULL) == SQLITE_OK);
        assert(sqlite3_close(database) == SQLITE_OK);
        assert(chmod(f.database, 0600) == 0);
    }
    f.agent.database = f.database;
    af_open(&f.agent, 2, 4, 2);
    assert(f.agent.engine->utf8_store == !utf16);
    enable(&f, 1024);
    af_create(&f.agent, "Discover and load a skill, then delegate independently");
    af_run(&f.agent);
    assert(f.agent.phase == 7 && f.agent.child_started == 1);
    af_state(&f.agent, f.agent.root, "completed");
    lifecycle(&f);
    af_close(&f.agent);
    af_remove_database(f.database);
    free(f.catalog);
    free(f.previous_request);
    assert(unlink(f.primary) == 0 && unlink(f.secondary) == 0 && unlink(filename) == 0);
    for (size_t i = 3; i > 0; --i) {
        path(filename, sizeof(filename), f.home, directories[i - 1]);
        assert(rmdir(filename) == 0);
    }
    assert(rmdir(f.home) == 0);
}

int main(void) {
    disabled_is_inert();
    runtime_and_context(false);
    runtime_and_context(true);
    puts("extension runtime/context tests passed");
    return 0;
}
