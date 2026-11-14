#include "catalog_private.h"
#include <stdio.h>

static bool root_suffix_safe(const char *text) {
    int depth = 0;
    while (*text == '/') {
        ++text;
        const char *end = text;
        while (*end && *end != '/' && *end != ' ' && *end != '\'' && *end != '"')
            ++end;
        size_t n = (size_t)(end - text);
        if (n == 2 && !memcmp(text, "..", 2)) {
            if (--depth < 0)
                return false;
        } else if (n && !(n == 1 && *text == '.'))
            ++depth;
        text = end;
    }
    return true;
}

char *ext_expand(const char *input, const char *root) {
    if (!root)
        return ah_strdup(input);
    const char *tokens[] = {"${CLAUDE_PLUGIN_ROOT}", "${CODEX_PLUGIN_ROOT}", "${PLUGIN_ROOT}"};
    size_t size = 1;
    for (const char *p = input; *p;) {
        size_t n = 0;
        for (size_t i = 0; i < 3; ++i)
            if (!strncmp(p, tokens[i], strlen(tokens[i])))
                n = strlen(tokens[i]);
        if (n && !root_suffix_safe(p + n))
            return NULL;
        size += n ? strlen(root) : 1;
        if (size > EXT_METADATA_MAX)
            return NULL;
        p += n ? n : 1;
    }
    char *output = malloc(size), *dest = output;
    if (!output)
        return NULL;
    for (const char *p = input; *p;) {
        size_t n = 0;
        for (size_t i = 0; i < 3; ++i)
            if (!strncmp(p, tokens[i], strlen(tokens[i])))
                n = strlen(tokens[i]);
        if (n) {
            size_t len = strlen(root);
            memcpy(dest, root, len);
            dest += len;
            p += n;
        } else
            *dest++ = *p++;
    }
    *dest = 0;
    return output;
}

static yyjson_mut_val *expanded_value(yyjson_mut_doc *doc, yyjson_val *value, const char *root,
                                      unsigned depth) {
    if (depth > 12)
        return NULL;
    if (yyjson_is_str(value)) {
        char *text = ext_expand(yyjson_get_str(value), root);
        yyjson_mut_val *out = text ? yyjson_mut_strcpy(doc, text) : NULL;
        free(text);
        return out;
    }
    if (yyjson_is_arr(value)) {
        yyjson_mut_val *out = yyjson_mut_arr(doc);
        size_t i, n;
        yyjson_val *item;
        yyjson_arr_foreach(value, i, n, item) {
            yyjson_mut_val *copy = expanded_value(doc, item, root, depth + 1);
            if (!copy || !yyjson_mut_arr_append(out, copy))
                return NULL;
        }
        return out;
    }
    if (yyjson_is_obj(value)) {
        yyjson_mut_val *out = yyjson_mut_obj(doc);
        yyjson_obj_iter it = yyjson_obj_iter_with(value);
        yyjson_val *key;
        while ((key = yyjson_obj_iter_next(&it))) {
            yyjson_mut_val *copy =
                expanded_value(doc, yyjson_obj_iter_get_val(key), root, depth + 1);
            if (!copy || !yyjson_mut_obj_add_val(doc, out, yyjson_get_str(key), copy))
                return NULL;
        }
        return out;
    }
    return yyjson_val_mut_copy(doc, value);
}

static toml_datum_t plugin_policy(ext_catalog *c, const char *plugin, const char *name,
                                  const char *key) {
    toml_datum_t value = {0};
    if (!plugin)
        return value;
    char bare[257];
    size_t length = strcspn(plugin, "@");
    if (length >= sizeof(bare))
        return value;
    memcpy(bare, plugin, length);
    bare[length] = 0;
    for (size_t i = 0; i < 2; ++i) {
        const char *ids[] = {bare, plugin};
        for (size_t j = 0; j < 2; ++j) {
            toml_datum_t rule = toml_get(toml_get(c->config[i].toptab, "plugins"), ids[j]);
            rule = toml_get(toml_get(rule, "mcp_servers"), name);
            toml_datum_t candidate = toml_get(rule, key);
            if (candidate.type != TOML_UNKNOWN)
                value = candidate;
        }
    }
    return value;
}

static bool string_array(yyjson_val *value) {
    if (!value)
        return true;
    if (!yyjson_is_arr(value))
        return false;
    size_t i, n;
    yyjson_val *item;
    yyjson_arr_foreach(value, i, n, item) if (!yyjson_is_str(item)) return false;
    return true;
}

static bool string_map(yyjson_val *value) {
    if (!value)
        return true;
    if (!yyjson_is_obj(value))
        return false;
    yyjson_obj_iter it = yyjson_obj_iter_with(value);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&it)))
        if (!yyjson_is_str(yyjson_obj_iter_get_val(key)))
            return false;
    return true;
}

ah_code ext_server_add(ext_catalog *c, const char *name, const char *plugin, yyjson_val *input,
                       const char *plugin_root, ah_error *error) {
    if (!yyjson_is_obj(input))
        return ah_fail(error, AH_INVALID, "MCP server must be an object");
    const char *strings[] = {"command",          "url",  "cwd",
                             "protocol_version", "type", "bearer_token_env_var",
                             "environment_id"};
    for (size_t i = 0; i < sizeof(strings) / sizeof(*strings); ++i) {
        yyjson_val *value = yyjson_obj_get(input, strings[i]);
        if (value && (!yyjson_is_str(value) || !yyjson_get_len(value)))
            return ah_fail(error, AH_INVALID, "MCP transport fields must be nonempty strings");
    }
    const char *arrays[] = {"args", "env_vars", "enabled_tools", "disabled_tools"};
    for (size_t i = 0; i < 4; ++i)
        if (!string_array(yyjson_obj_get(input, arrays[i])))
            return ah_fail(error, AH_INVALID,
                           "MCP argument and policy lists must contain only strings");
    const char *maps[] = {"env", "headers", "http_headers", "env_http_headers"};
    for (size_t i = 0; i < 4; ++i)
        if (!string_map(yyjson_obj_get(input, maps[i])))
            return ah_fail(error, AH_INVALID,
                           "MCP environment and header maps must contain only strings");
    yyjson_val *enabled_value = yyjson_obj_get(input, "enabled");
    if (enabled_value && !yyjson_is_bool(enabled_value))
        return ah_fail(error, AH_INVALID, "MCP enabled must be a boolean");
    toml_datum_t plugin_enabled = plugin_policy(c, plugin, name, "enabled");
    if (plugin_enabled.type != TOML_UNKNOWN && plugin_enabled.type != TOML_BOOLEAN)
        return ah_fail(error, AH_INVALID, "Plugin MCP enabled must be a boolean");
    const char *filters[] = {"enabled_tools", "disabled_tools"};
    for (size_t i = 0; i < 2; ++i) {
        toml_datum_t policy = plugin_policy(c, plugin, name, filters[i]);
        if (policy.type == TOML_UNKNOWN)
            continue;
        if (policy.type != TOML_ARRAY)
            return ah_fail(error, AH_INVALID, "Plugin tool filters must be string arrays");
        for (int j = 0; j < policy.u.arr.size; ++j)
            if (policy.u.arr.elem[j].type != TOML_STRING)
                return ah_fail(error, AH_INVALID, "Plugin tool filters must be string arrays");
    }
    char *key = plugin ? ext_join(plugin, name) : ah_strdup(name);
    char *id = key ? ext_id("mcp", key) : NULL;
    if (!id) {
        free(key);
        return ah_fail(error, AH_IO, "Out of memory");
    }
    bool enabled = plugin_enabled.type == TOML_BOOLEAN
                       ? plugin_enabled.u.boolean
                       : !yyjson_is_false(yyjson_obj_get(input, "enabled"));
    enabled = enabled && !ext_disabled(c, "mcp", plugin ? "" : name, "");
    const char *reason = enabled ? "" : "disabled by configuration";
    yyjson_val *command = yyjson_obj_get(input, "command"), *url = yyjson_obj_get(input, "url");
    bool available = yyjson_is_str(command) != yyjson_is_str(url);
    const char *type = yyjson_get_str(yyjson_obj_get(input, "type"));
    if (type && strcmp(type, "stdio") && strcmp(type, "http") && strcmp(type, "streamable-http") &&
        strcmp(type, "streamable_http")) {
        available = false;
        reason = "unsupported MCP transport";
    }
    yyjson_val *extension = yyjson_obj_get(yyjson_obj_get(input, "extensions"), "com.openai");
    yyjson_val *auth = yyjson_obj_get(extension, "auth");
    const char *auth_type = yyjson_get_str(yyjson_obj_get(auth, "type"));
    yyjson_val *direct_auth = yyjson_obj_get(input, "auth");
    const char *direct_auth_type = yyjson_is_str(direct_auth)
                                       ? yyjson_get_str(direct_auth)
                                       : yyjson_get_str(yyjson_obj_get(direct_auth, "type"));
    const char *environment = yyjson_get_str(yyjson_obj_get(input, "environment_id"));
    if ((auth_type && strcmp(auth_type, "none")) ||
        (direct_auth && (!direct_auth_type || strcmp(direct_auth_type, "none"))) ||
        yyjson_obj_get(input, "oauth") || yyjson_obj_get(input, "connector_id")) {
        available = false;
        reason = "hosted authentication or connector runtime unavailable";
    }
    if (yyjson_obj_get(input, "http_headers_helper") ||
        (environment && strcmp(environment, "local")) ||
        yyjson_obj_get(input, "transport_constraints")) {
        available = false;
        reason = "required MCP host behavior unavailable";
    }
    if (!available && !*reason)
        reason = "MCP requires exactly one command or URL";
    static const char *fields[] = {"command",
                                   "args",
                                   "cwd",
                                   "env",
                                   "env_vars",
                                   "url",
                                   "http_headers",
                                   "env_http_headers",
                                   "bearer_token_env_var",
                                   "startup_timeout_sec",
                                   "startup_timeout_ms",
                                   "tool_timeout_sec",
                                   "enabled_tools",
                                   "disabled_tools",
                                   "protocol_version"};
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    bool good = true;
    for (size_t i = 0; i < sizeof(fields) / sizeof(*fields); ++i) {
        if (!strcmp(fields[i], "enabled_tools") || !strcmp(fields[i], "disabled_tools")) {
            toml_datum_t policy = plugin_policy(c, plugin, name, fields[i]);
            if (policy.type != TOML_UNKNOWN) {
                yyjson_mut_val *copy = ext_toml_json(doc, policy, 0);
                if (!copy || !yyjson_mut_obj_add_val(doc, root, fields[i], copy))
                    good = false;
                continue;
            }
        }
        yyjson_val *value = yyjson_obj_get(input, fields[i]);
        if (!value && !strcmp(fields[i], "http_headers"))
            value = yyjson_obj_get(input, "headers");
        if (!value)
            continue;
        yyjson_mut_val *copy = NULL;
        if (plugin_root && !strcmp(fields[i], "cwd") && yyjson_is_str(value)) {
            char *expanded = ext_expand(yyjson_get_str(value), plugin_root);
            char *path =
                expanded && expanded[0] != '/' ? ext_join(plugin_root, expanded) : expanded;
            if (path != expanded)
                free(expanded);
            char *canonical = path ? realpath(path, NULL) : NULL;
            if (canonical && ext_inside(plugin_root, canonical))
                copy = yyjson_mut_strcpy(doc, canonical);
            free(path);
            free(canonical);
        } else
            copy = expanded_value(doc, value, plugin_root, 0);
        if (!copy || !yyjson_mut_obj_add_val(doc, root, fields[i], copy))
            good = false;
    }
    if (plugin_root && yyjson_is_str(command) && !yyjson_obj_get(input, "cwd"))
        yyjson_mut_obj_add_strcpy(doc, root, "cwd", plugin_root);
    char *json = good ? ah_mut_write(doc) : NULL;
    ah_mut_doc_free(doc);
    ext_entry entry = {.id = id,
                       .kind = "mcp",
                       .name = (char *)name,
                       .path = key,
                       .plugin = (char *)plugin,
                       .enabled = enabled,
                       .available = available,
                       .reason = (char *)reason,
                       .config = json};
    ah_code rc = json ? ext_add(c, &entry, error)
                      : ah_fail(error, AH_INVALID,
                                "Cannot normalize MCP configuration or plugin path escapes root");
    free(json);
    free(key);
    free(id);
    return rc;
}
