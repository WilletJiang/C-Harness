#include "extensions_internal.h"
#include <ctype.h>

static const char discover_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"kind\":{\"type\":\"string\",\"enum\":[\"all\",\"skill\",\"mcp\",\"plugin\"]},"
    "\"query\":{\"type\":\"string\"},\"plugin\":{\"type\":\"string\"},"
    "\"offset\":{\"type\":\"integer\",\"minimum\":0},"
    "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":32}},"
    "\"additionalProperties\":false}";
static const char skill_schema[] =
    "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},"
    "\"action\":{\"type\":\"string\",\"enum\":[\"load\",\"unload\",\"read\"]},"
    "\"path\":{\"type\":\"string\"},\"offset\":{\"type\":\"integer\",\"minimum\":0},"
    "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":65536}},"
    "\"required\":[\"id\"],\"additionalProperties\":false}";
static const char mcp_schema[] =
    "{\"type\":\"object\",\"properties\":{\"server\":{\"type\":\"string\"},"
    "\"action\":{\"type\":\"string\",\"enum\":[\"tools\",\"describe\",\"resources\","
    "\"read\",\"templates\",\"prompts\",\"get\"]},\"name\":{\"type\":\"string\"},"
    "\"uri\":{\"type\":\"string\"},\"arguments\":{\"type\":\"object\"},"
    "\"query\":{\"type\":\"string\"},\"cursor\":{\"type\":\"string\"},"
    "\"offset\":{\"type\":\"integer\",\"minimum\":0},"
    "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":32},"
    "\"refresh\":{\"type\":\"boolean\"}},\"required\":[\"server\",\"action\"],"
    "\"additionalProperties\":false}";
static const char call_schema[] =
    "{\"type\":\"object\",\"properties\":{\"server\":{\"type\":\"string\"},"
    "\"name\":{\"type\":\"string\"},\"arguments\":{\"type\":\"object\"}},"
    "\"required\":[\"server\",\"name\",\"arguments\"],\"additionalProperties\":false}";

const char *ext_string(yyjson_val *root, const char *key) {
    return yyjson_get_str(yyjson_obj_get(root, key));
}
size_t ext_number(yyjson_val *root, const char *key, size_t fallback) {
    yyjson_val *v = yyjson_obj_get(root, key);
    if (!v)
        return fallback;
    double number = yyjson_get_num(v);
    return number >= (double)SIZE_MAX ? SIZE_MAX : number < 0 ? fallback : (size_t)number;
}
bool ext_match(const char *text, const char *query) {
    if (!query || !*query)
        return true;
    if (!text)
        return false;
    size_t n = strlen(query);
    for (; *text; ++text) {
        size_t i = 0;
        while (i < n && text[i] &&
               tolower((unsigned char)text[i]) == tolower((unsigned char)query[i]))
            ++i;
        if (i == n)
            return true;
    }
    return false;
}
ah_code ext_result(yyjson_mut_doc *doc, ah_tool_result *result, ah_error *error) {
    result->json = ah_mut_write(doc);
    return result->json ? AH_OK : ah_fail(error, AH_IO, "cannot encode extension result");
}
ah_code ext_error(ah_tool_result *result, ah_code code, ah_error *error) {
    result->is_error = true;
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "code", ah_code_name(code));
    yyjson_mut_obj_add_strcpy(doc, root, "error",
                              error && *error->message ? error->message : "extension failed");
    ah_code rc = ext_result(doc, result, error);
    ah_mut_doc_free(doc);
    return rc;
}
static ah_code discover(void *context, const ah_tool_call *call, ah_tool_result *result,
                        ah_error *error) {
    ah_extensions *x = context;
    yyjson_doc *doc = yyjson_read(call->arguments_json, strlen(call->arguments_json), 0);
    yyjson_val *root = yyjson_doc_get_root(doc);
    const char *kind = ext_string(root, "kind");
    ah_code rc = ext_catalog_query(x->catalog, kind ? kind : "all", ext_string(root, "query"),
                                   ext_string(root, "plugin"), ext_number(root, "offset", 0),
                                   ext_number(root, "limit", 12), &result->json, error);
    yyjson_doc_free(doc);
    return rc;
}
void ah_extensions_config_init(ah_extensions_config *config) {
    if (!config)
        return;
    memset(config, 0, sizeof(*config));
    config->approve_mcp_calls = true;
    config->max_catalog_entries = 2048;
    config->max_catalog_bytes = 8 * 1024 * 1024;
    config->max_skill_bytes = 64 * 1024;
    config->max_context_bytes = 128 * 1024;
    config->max_result_bytes = 1024 * 1024;
}
ah_code ah_extensions_enable(ah_engine *e, const ah_extensions_config *config, ah_error *error) {
    ah_code rc = core_guard(e, error);
    if (rc != AH_OK)
        return rc;
    if (!config)
        return ah_fail(error, AH_INVALID, "extension configuration required");
    if (!config->enable_skills && !config->enable_mcp && !config->enable_plugins)
        return AH_OK;
    if (e->extensions)
        return ah_fail(error, AH_BUSY, "extensions already enabled");
    if (!config->max_catalog_entries || config->max_catalog_entries > 10000 ||
        config->max_catalog_bytes < 1024 || config->max_catalog_bytes > 64 * 1024 * 1024 ||
        !config->max_skill_bytes || config->max_skill_bytes > 1024 * 1024 ||
        config->max_context_bytes < 1024 ||
        config->max_context_bytes > e->config.max_context_bytes ||
        config->max_result_bytes < 1024 || config->max_result_bytes > 16 * 1024 * 1024)
        return ah_fail(error, AH_INVALID, "extension budgets outside supported bounds");
    ah_extensions *x = calloc(1, sizeof(*x));
    if (!x)
        return ah_fail(error, AH_IO, "out of memory");
    x->engine = e;
    x->config = *config;
    rc = ext_catalog_open(config, &x->catalog, error);
    /* Catalog owns copies of paths; no other extension code retains them. */
    x->config.codex_home = x->config.workspace = NULL;
    ah_tool tools[] = {
        {.name = "discover",
         .description =
             "Find opted-in skills, MCP servers and plugin bundles. "
             "Returns paginated metadata only. Set plugin to a discovered plugin ID to expand just "
             "that bundle; members still require their feature enabled. Default limit 12.",
         .parameters_json = discover_schema,
         .effect = AH_EFFECT_READ,
         .execute = discover,
         .context = x},
        {.name = "skill",
         .description = "Load one discovered skill into this session's bounded "
                        "reference context (default action load), or unload it. Repeated loads "
                        "replace the same "
                        "slot; instructions survive compaction. Use action read with a relative "
                        "path and optional "
                        "offset/limit to page a UTF-8 reference within the skill directory. Files "
                        "load on demand.",
         .parameters_json = skill_schema,
         .effect = AH_EFFECT_READ,
         .execute = ext_skill,
         .context = x},
        {.name = "mcp",
         .description =
             "Inspect one opted-in MCP server on demand. tools/resources/"
             "prompts/templates return metadata, describe returns one tool schema, read gets a "
             "resource by uri, "
             "get gets a prompt by name and arguments. Lists use offset/limit within a "
             "server cursor "
             "page; pass next_cursor after that page. tools is cached until "
             "refresh=true. No calls.",
         .parameters_json = mcp_schema,
         .effect = AH_EFFECT_READ,
         .execute = ext_mcp,
         .context = x},
        {.name = "mcp_call",
         .description =
             "Call a discovered MCP tool. First inspect its schema "
             "with mcp describe. Arguments are passed as an object. External effects follow host "
             "approval; an uncertain request is never automatically repeated.",
         .parameters_json = call_schema,
         .effect = AH_EFFECT_EXTERNAL,
         .requires_approval = config->approve_mcp_calls,
         .execute = ext_mcp_call,
         .context = x}};
    size_t before = e->tool_count;
    for (size_t i = 0; i < sizeof(tools) / sizeof(*tools) && rc == AH_OK; ++i) {
        if ((i == 1 && !config->enable_skills) || (i >= 2 && !config->enable_mcp))
            continue;
        if (core_find_tool(e, tools[i].name))
            rc = ah_fail(error, AH_INVALID, "extension tool name already registered");
        else
            rc = ah_register_tool(e, &tools[i], error);
    }
    if (rc != AH_OK) {
        while (e->tool_count > before) {
            core_tool *tool = &e->tools[--e->tool_count];
            free(tool->name);
            free(tool->description);
            free(tool->schema);
        }
        ext_close(x);
        return rc;
    }
    e->extensions = x;
    return AH_OK;
}
void ext_close(ah_extensions *x) {
    if (!x)
        return;
    ext_server *server = x->servers;
    while (server) {
        ext_server *next = server->next;
        mcp_client_close(server->client);
        yyjson_doc_free(server->config);
        yyjson_doc_free(server->tools);
        yyjson_doc_free(server->resources);
        yyjson_doc_free(server->prompts);
        yyjson_doc_free(server->templates);
        free(server->tools_cursor);
        free(server->resources_cursor);
        free(server->prompts_cursor);
        free(server->templates_cursor);
        free(server->id);
        free(server);
        server = next;
    }
    ext_catalog_close(x->catalog);
    free(x);
}
