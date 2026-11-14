#include "extensions/catalog_private.h"
#include <assert.h>
#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static char fixture[PATH_MAX];

static void write_file(const char *relative, const char *text) {
    char *path = ext_join(fixture, relative);
    assert(path);
    for (char *p = path + 1; *p; ++p)
        if (*p == '/') {
            *p = 0;
            assert(!mkdir(path, 0700) || errno == EEXIST);
            *p = '/';
        }
    FILE *file = fopen(path, "w");
    assert(file && fputs(text, file) >= 0 && !fclose(file));
    free(path);
}

static ext_catalog *open_catalog(bool skills, bool mcp, bool plugins, size_t count) {
    ah_extensions_config options = {.codex_home = fixture,
                                    .enable_skills = skills,
                                    .enable_mcp = mcp,
                                    .enable_plugins = plugins,
                                    .max_catalog_entries = count};
    ext_catalog *catalog = NULL;
    ah_error error = {0};
    assert(ext_catalog_open(&options, &catalog, &error) == AH_OK);
    assert(catalog && catalog->bytes == 0 && catalog->count == 0);
    return catalog;
}

static yyjson_doc *query(ext_catalog *catalog, const char *kind, const char *term,
                         const char *plugin, size_t offset, size_t limit) {
    char *text = NULL;
    ah_error error = {0};
    ah_code rc = ext_catalog_query(catalog, kind, term, plugin, offset, limit, &text, &error);
    if (rc != AH_OK)
        fprintf(stderr, "catalog query: %s\n", error.message);
    assert(rc == AH_OK && text);
    assert(!strstr(text, "super-secret") && !strstr(text, "PRIVATE_BODY"));
    yyjson_doc *doc = yyjson_read(text, strlen(text), 0);
    free(text);
    assert(doc);
    return doc;
}

static size_t total(yyjson_doc *doc) {
    return (size_t)yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(doc), "total"));
}

static yyjson_val *first(yyjson_doc *doc) {
    return yyjson_arr_get(yyjson_obj_get(yyjson_doc_get_root(doc), "items"), 0);
}

static void test_lazy_and_config(void) {
    write_file("config.toml", "broken = [\n");
    ext_catalog *catalog = open_catalog(false, false, false, 100);
    yyjson_doc *doc = query(catalog, "all", NULL, NULL, 0, 20);
    assert(total(doc) == 0 && catalog->bytes == 0);
    yyjson_doc_free(doc);
    ext_catalog_close(catalog);
    catalog = open_catalog(true, false, false, 100);
    char *json = NULL;
    ah_error error = {0};
    assert(ext_catalog_query(catalog, "skill", NULL, NULL, 0, 20, &json, &error) == AH_INVALID);
    assert(!json);
    ext_catalog_close(catalog);
    write_file("config.toml",
               "[[skills.config]]\nname = 'disabled'\nenabled = false\n"
               "[mcp_servers.\"quoted.server\"]\ncommand = \"super-secret-command\"\n"
               "args = [\"one\", \"two\"]\n[mcp_servers.\"quoted.server\".env]\nTOKEN = "
               "'super-secret-token'\n"
               "[mcp_servers.off]\ncommand = 'false'\nenabled = false\n"
               "[plugins.\"off@market\"]\nenabled = false\n"
               "[plugins.\"legacy@market\".mcp_servers.local]\nenabled_tools = "
               "['allowed']\ndisabled_tools = ['denied']\n");
    write_file(
        "skills/.system/one/SKILL.md",
        "---\nname: duplicate\ndescription: >-\n  First line\n  second line\n---\nPRIVATE_BODY\n");
    write_file("skills/two/SKILL.md",
               "---\nname: 'duplicate'\ndescription: \"Quoted description\"\n---\nPRIVATE_BODY\n");
    write_file("skills/disabled/SKILL.md",
               "---\nname: disabled\ndescription: Test disabling\n---\nPRIVATE_BODY\n");
    char *loop = ext_join(fixture, "skills/loop");
    assert(loop && symlink(".", loop) == 0);
    free(loop);
    catalog = open_catalog(true, true, false, 100);
    doc = query(catalog, "skill", "duplicate", NULL, 0, 1);
    assert(total(doc) == 2);
    assert(yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(doc), "next_offset")) == 1);
    char *id = ah_strdup(yyjson_get_str(yyjson_obj_get(first(doc), "id")));
    yyjson_doc_free(doc);
    doc = query(catalog, "skill", "duplicate", NULL, 1, 1);
    assert(strcmp(id, yyjson_get_str(yyjson_obj_get(first(doc), "id"))));
    yyjson_doc_free(doc);
    char *path = NULL;
    assert(ext_catalog_skill(catalog, id, &path, &error) == AH_OK && path[0] == '/');
    free(path);
    free(id);
    doc = query(catalog, "skill", "disabled", NULL, 0, 1);
    assert(!yyjson_get_bool(yyjson_obj_get(first(doc), "enabled")));
    assert(ext_catalog_skill(catalog, yyjson_get_str(yyjson_obj_get(first(doc), "id")), &path,
                             &error) == AH_INVALID);
    yyjson_doc_free(doc);
    doc = query(catalog, "mcp", "quoted.server", NULL, 0, 1);
    assert(total(doc) == 1);
    assert(ext_catalog_server(catalog, yyjson_get_str(yyjson_obj_get(first(doc), "id")), &json,
                              &error) == AH_OK);
    assert(strstr(json, "super-secret-token"));
    free(json);
    yyjson_doc_free(doc);
    doc = query(catalog, "mcp", "off", NULL, 0, 1);
    assert(ext_catalog_server(catalog, yyjson_get_str(yyjson_obj_get(first(doc), "id")), &json,
                              &error) == AH_INVALID);
    yyjson_doc_free(doc);
    ext_catalog_close(catalog);
}

static void test_plugins(void) {
    write_file("plugins/cache/market/legacy/1.9/.codex-plugin/plugin.json", "{\"name\":\"old\"}");
    write_file("plugins/cache/market/legacy/1.10/.codex-plugin/plugin.json",
               "{\"name\":\"legacy\",\"skills\":\"./skills\",\"mcpServers\":\"./.mcp.json\"}");
    write_file("plugins/cache/market/legacy/1.10/skills/test/SKILL.md",
               "---\nname: plugin-skill\ndescription: Plugin example\n---\nPRIVATE_BODY\n");
    write_file("plugins/cache/market/legacy/1.10/.mcp.json",
               "{\"mcpServers\":{\"local\":{\"command\":\"${CLAUDE_PLUGIN_ROOT}/"
               "server\",\"args\":[\"${CODEX_PLUGIN_ROOT}\",\"${PLUGIN_ROOT}\"],\"env\":{\"TOKEN\":"
               "\"super-secret\"}}}}");
    write_file("plugins/cache/market/portable/2/plugin.json",
               "{\"name\":\"portable\",\"description\":\"Portable example\"}");
    write_file("plugins/cache/market/portable/2/.codex-plugin/plugin.json",
               "{\"name\":\"overlay\",\"apps\":\"./.app.json\"}");
    write_file("plugins/cache/market/portable/2/mcp.json",
               "{\"mcpServers\":{\"remote\":{\"type\":\"streamable-http\",\"url\":\"https://"
               "example.com/mcp\"}}}");
    write_file("plugins/cache/market/off/1/plugin.json", "{\"name\":\"off\"}");
    write_file("plugins/cache/market/escape/1/.codex-plugin/plugin.json",
               "{\"name\":\"escape\",\"skills\":\"../../../../../skills\"}");
    write_file("plugins/cache/market/malformed/1/plugin.json", "{malformed");
    write_file("bundled/linked/plugin.json", "{\"name\":\"linked\"}");
    write_file("plugins/cache/market/linked/placeholder", "");
    char *link = ext_join(fixture, "plugins/cache/market/linked/latest");
    char *target = ext_join(fixture, "bundled/linked");
    assert(!symlink(target, link));
    free(link);
    free(target);
    ext_catalog *catalog = open_catalog(true, true, true, 100);
    yyjson_doc *doc = query(catalog, "plugin", NULL, NULL, 0, 20);
    assert(total(doc) == 6 && catalog->count == 6);
    yyjson_doc_free(doc);
    doc = query(catalog, "all", NULL, "legacy@market", 0, 20);
    assert(total(doc) == 3);
    assert(!catalog->skills_loaded && !catalog->servers_loaded);
    yyjson_doc_free(doc);
    doc = query(catalog, "mcp", NULL, "legacy@market", 0, 20);
    char *config = NULL;
    ah_error error = {0};
    assert(ext_catalog_server(catalog, yyjson_get_str(yyjson_obj_get(first(doc), "id")), &config,
                              &error) == AH_OK);
    assert(!strstr(config, "${") && strstr(config, "/1.10/server"));
    assert(strstr(config, "enabled_tools") && strstr(config, "allowed") &&
           strstr(config, "denied"));
    free(config);
    yyjson_doc_free(doc);
    doc = query(catalog, "all", NULL, "portable@market", 0, 20);
    assert(total(doc) == 2);
    yyjson_doc_free(doc);
    char *json = NULL;
    assert(ext_catalog_query(catalog, "all", NULL, "off@market", 0, 20, &json, &error) ==
           AH_INVALID);
    assert(ext_catalog_query(catalog, "all", NULL, "escape@market", 0, 20, &json, &error) ==
           AH_INVALID);
    doc = query(catalog, "plugin", NULL, "linked@market", 0, 20);
    assert(total(doc) == 1);
    yyjson_doc_free(doc);
    doc = query(catalog, "plugin", "malformed", NULL, 0, 20);
    assert(total(doc) == 1 && !yyjson_get_bool(yyjson_obj_get(first(doc), "available")));
    yyjson_doc_free(doc);
    ext_catalog_close(catalog);
    catalog = open_catalog(false, false, true, 100);
    doc = query(catalog, "all", NULL, "legacy@market", 0, 20);
    assert(total(doc) == 1);
    yyjson_doc_free(doc);
    ext_catalog_close(catalog);
}

static void test_workspace_and_policy(void) {
    write_file("workspace/.git", "gitdir: unused\n");
    write_file("workspace/.agents/skills/ancestor/SKILL.md",
               "---\nname: ancestor\ndescription: At repository root\n---\nPRIVATE_BODY\n");
    write_file("workspace/sub/.agents/skills/nested/SKILL.md",
               "---\nname: nested\ndescription: At selected directory\n---\nPRIVATE_BODY\n");
    char *workspace = ext_join(fixture, "workspace/sub");
    char *disabled = ext_join(fixture, "workspace/.agents/skills/ancestor");
    char config[PATH_MAX + 600];
    snprintf(config, sizeof(config),
             "[[skills.config]]\npath = '%s'\nenabled = false\n"
             "[[skills.config]]\nname = 'disabled'\nenabled = true\n"
             "[[skills.config]]\nname = 'nested'\nenabled = false\n"
             "[[skills.config]]\nname = 'nested'\nenabled = true\n"
             "[skills.bundled]\nenabled = false\n"
             "[mcp_servers.off]\nenabled = true\n"
             "[mcp_servers.\"quoted.server\"]\ntool_timeout_sec = 4\n",
             disabled);
    write_file("workspace/sub/.codex/config.toml", config);
    free(disabled);
    ah_extensions_config options = {.codex_home = fixture,
                                    .workspace = workspace,
                                    .enable_skills = true,
                                    .enable_mcp = true,
                                    .max_catalog_entries = 100};
    ext_catalog *catalog = NULL;
    ah_error error = {0};
    assert(ext_catalog_open(&options, &catalog, &error) == AH_OK);
    yyjson_doc *doc = query(catalog, "skill", "ancestor", NULL, 0, 20);
    assert(total(doc) == 1 && !yyjson_get_bool(yyjson_obj_get(first(doc), "enabled")));
    yyjson_doc_free(doc);
    doc = query(catalog, "skill", "nested", NULL, 0, 20);
    assert(total(doc) == 1 && yyjson_get_bool(yyjson_obj_get(first(doc), "enabled")));
    yyjson_doc_free(doc);
    doc = query(catalog, "skill", "disabled", NULL, 0, 20);
    assert(total(doc) == 1 && yyjson_get_bool(yyjson_obj_get(first(doc), "enabled")));
    yyjson_doc_free(doc);
    doc = query(catalog, "skill", "First line", NULL, 0, 20);
    assert(total(doc) == 1 && !yyjson_get_bool(yyjson_obj_get(first(doc), "enabled")));
    yyjson_doc_free(doc);
    doc = query(catalog, "mcp", "off", NULL, 0, 20);
    assert(total(doc) == 1 && yyjson_get_bool(yyjson_obj_get(first(doc), "enabled")));
    yyjson_doc_free(doc);
    doc = query(catalog, "mcp", "quoted.server", NULL, 0, 20);
    char *json = NULL;
    assert(ext_catalog_server(catalog, yyjson_get_str(yyjson_obj_get(first(doc), "id")), &json,
                              &error) == AH_OK);
    assert(strstr(json, "super-secret-command") && strstr(json, "tool_timeout_sec"));
    free(json);
    yyjson_doc_free(doc);
    ext_catalog_close(catalog);
    write_file("workspace/sub/.codex/config.toml",
               "[mcp_servers.\"quoted.server\"]\ndisabled_tools = [true]\n");
    assert(ext_catalog_open(&options, &catalog, &error) == AH_OK);
    assert(ext_catalog_query(catalog, "mcp", NULL, NULL, 0, 20, &json, &error) == AH_INVALID);
    assert(ext_catalog_query(catalog, "mcp", NULL, NULL, 0, 20, &json, &error) == AH_INVALID);
    ext_catalog_close(catalog);
    write_file(
        "workspace/sub/.codex/config.toml",
        "[mcp_servers.\"quoted.server\"]\nauth = 'chatgpt'\nprotocol_version = '2020-01-01'\n");
    assert(ext_catalog_open(&options, &catalog, &error) == AH_OK);
    doc = query(catalog, "mcp", "quoted.server", NULL, 0, 20);
    assert(!yyjson_get_bool(yyjson_obj_get(first(doc), "available")));
    assert(ext_catalog_server(catalog, yyjson_get_str(yyjson_obj_get(first(doc), "id")), &json,
                              &error) == AH_INVALID);
    yyjson_doc_free(doc);
    ext_catalog_close(catalog);
    free(workspace);
}

static void test_scale(void) {
    char *large = ext_join(fixture, "skills/two/SKILL.md");
    FILE *body = fopen(large, "a");
    assert(body);
    for (unsigned i = 0; i < 100000; ++i)
        assert(fputs("PRIVATE_BODY\n", body) >= 0);
    assert(!fclose(body));
    free(large);
    for (unsigned i = 0; i < 1000; ++i) {
        char path[100], text[160];
        snprintf(path, sizeof(path), "skills/scale-%04u/SKILL.md", i);
        snprintf(text, sizeof(text),
                 "---\nname: scale-%04u\ndescription: Bounded metadata\n---\nPRIVATE_BODY\n", i);
        write_file(path, text);
    }
    ext_catalog *catalog = open_catalog(true, false, false, 1200);
    yyjson_doc *doc = query(catalog, "skill", "scale-", NULL, 985, 15);
    assert(total(doc) == 1000);
    assert(yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(doc), "items")) == 15);
    assert(catalog->bytes < 100000 && catalog->retained < 1000000);
    yyjson_doc_free(doc);
    size_t bytes = catalog->bytes;
    doc = query(catalog, "skill", "scale-", NULL, SIZE_MAX, 15);
    assert(catalog->bytes == bytes &&
           yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(doc), "items")) == 0);
    yyjson_doc_free(doc);
    ext_catalog_close(catalog);
    catalog = open_catalog(true, false, false, 10);
    char *json = NULL;
    ah_error error = {0};
    assert(ext_catalog_query(catalog, "skill", NULL, NULL, 0, 10, &json, &error) == AH_LIMIT);
    ext_catalog_close(catalog);
}

static int remove_path(const char *path, const struct stat *st, int type, struct FTW *walk) {
    (void)st;
    (void)type;
    (void)walk;
    return remove(path);
}

int main(void) {
    char temporary[] = "/tmp/ah-catalog-test-XXXXXX";
    assert(mkdtemp(temporary));
    assert(strlen(temporary) < sizeof(fixture));
    strcpy(fixture, temporary);
    assert(!setenv("HOME", fixture, 1));
    test_lazy_and_config();
    test_plugins();
    test_workspace_and_policy();
    test_scale();
    assert(!nftw(fixture, remove_path, 32, FTW_DEPTH | FTW_PHYS));
    puts("extension catalog tests passed");
    return 0;
}
