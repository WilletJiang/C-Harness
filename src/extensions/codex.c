#include "catalog_private.h"
#include <stdio.h>

static size_t datum_size(toml_datum_t value, unsigned depth) {
    if (depth > 32)
        return SIZE_MAX;
    size_t size = sizeof(value);
    if (value.type == TOML_STRING) {
        if (strlen(value.u.str.ptr) != (size_t)value.u.str.len)
            return SIZE_MAX;
        return size + (size_t)value.u.str.len + 1;
    }
    if (value.type == TOML_TABLE) {
        for (int i = 0; i < value.u.tab.size; ++i) {
            size_t child = datum_size(value.u.tab.value[i], depth + 1);
            if (child == SIZE_MAX || strlen(value.u.tab.key[i]) != (size_t)value.u.tab.len[i])
                return SIZE_MAX;
            size += strlen(value.u.tab.key[i]) + 1 + sizeof(char *) + sizeof(int) + child;
        }
    } else if (value.type == TOML_ARRAY) {
        for (int i = 0; i < value.u.arr.size; ++i) {
            size_t child = datum_size(value.u.arr.elem[i], depth + 1);
            if (child == SIZE_MAX)
                return SIZE_MAX;
            size += child;
        }
    }
    return size;
}

static bool enabled_valid(toml_datum_t value) {
    value = toml_get(value, "enabled");
    return value.type == TOML_UNKNOWN || value.type == TOML_BOOLEAN;
}

static bool policy_valid(toml_datum_t root) {
    const char *groups[] = {"skills", "plugins", "mcp", "mcp_servers"};
    for (size_t i = 0; i < 4; ++i) {
        toml_datum_t group = toml_get(root, groups[i]);
        if (group.type != TOML_UNKNOWN && group.type != TOML_TABLE)
            return false;
        if (!enabled_valid(group))
            return false;
        if ((i == 1 || i == 3) && group.type == TOML_TABLE) {
            for (int j = 0; j < group.u.tab.size; ++j) {
                if (!strcmp(group.u.tab.key[j], "enabled"))
                    continue;
                toml_datum_t child = group.u.tab.value[j];
                if (child.type != TOML_TABLE || !enabled_valid(child))
                    return false;
            }
        }
    }
    toml_datum_t rules = toml_seek(root, "skills.config");
    if (rules.type != TOML_UNKNOWN && rules.type != TOML_ARRAY)
        return false;
    if (rules.type == TOML_ARRAY)
        for (int i = 0; i < rules.u.arr.size; ++i) {
            toml_datum_t rule = rules.u.arr.elem[i];
            if (rule.type != TOML_TABLE || !enabled_valid(rule))
                return false;
        }
    return true;
}

static ah_code load_config(ext_catalog *c, const char *path, size_t index, ah_error *error) {
    char *text = NULL;
    ah_code rc = ext_read(c, path, EXT_METADATA_MAX, &text, error);
    if (rc == AH_NOT_FOUND)
        return AH_OK;
    if (rc != AH_OK)
        return rc;
    toml_result_t parsed = toml_parse(text, (int)strlen(text));
    free(text);
    if (!parsed.ok || !policy_valid(parsed.toptab)) {
        toml_free(parsed);
        return ah_fail(error, AH_INVALID, "Malformed Codex TOML configuration: %s", path);
    }
    size_t retained = datum_size(parsed.toptab, 0);
    size_t old = c->config[index].ok ? datum_size(c->config[index].toptab, 0) : 0;
    if (retained > ext_remaining(c) + old) {
        toml_free(parsed);
        return ah_fail(error, AH_LIMIT, "Extension configuration memory budget exceeded");
    }
    c->retained = c->retained - old + retained;
    toml_free(c->config[index]);
    c->config[index] = parsed;
    return AH_OK;
}

ah_code ext_config_load(ext_catalog *c, ah_error *error) {
    if (c->config_loaded)
        return AH_OK;
    char *path = ext_join(c->home, "config.toml");
    if (!path)
        return ah_fail(error, AH_IO, "Out of memory");
    ah_code rc = load_config(c, path, 0, error);
    free(path);
    if (rc == AH_OK && *c->workspace) {
        path = ext_join(c->workspace, ".codex/config.toml");
        if (!path)
            return ah_fail(error, AH_IO, "Out of memory");
        rc = load_config(c, path, 1, error);
        free(path);
    }
    if (rc == AH_OK)
        c->config_loaded = true;
    return rc;
}

static void apply_enabled(toml_datum_t value, bool *enabled) {
    if (value.type == TOML_BOOLEAN)
        *enabled = value.u.boolean;
}

static bool path_matches(ext_catalog *c, const char *configured, const char *path) {
    char *expanded = NULL;
    if (configured[0] == '~' && configured[1] == '/')
        expanded = ext_join(c->user_home, configured + 2);
    else
        expanded = ah_strdup(configured);
    if (!expanded)
        return false;
    char *canonical = realpath(expanded, NULL);
    const char *rule = canonical ? canonical : expanded;
    bool match = !strcmp(rule, path);
    size_t n = strlen(rule);
    if (!match && !strncmp(rule, path, n) && !strcmp(path + n, "/SKILL.md"))
        match = true;
    free(canonical);
    free(expanded);
    return match;
}

bool ext_disabled(ext_catalog *c, const char *kind, const char *name, const char *path) {
    bool group_enabled = true, item_enabled = true, bundled_enabled = true;
    for (size_t i = 0; i < 2; ++i) {
        toml_datum_t root = c->config[i].toptab;
        if (!strcmp(kind, "skill")) {
            toml_datum_t skills = toml_get(root, "skills");
            apply_enabled(toml_get(skills, "enabled"), &group_enabled);
            if (strstr(path, "/.system/"))
                apply_enabled(toml_seek(skills, "bundled.enabled"), &bundled_enabled);
            toml_datum_t rules = toml_get(skills, "config");
            if (rules.type != TOML_ARRAY)
                continue;
            for (int j = 0; j < rules.u.arr.size; ++j) {
                toml_datum_t rule = rules.u.arr.elem[j];
                toml_datum_t rn = toml_get(rule, "name"), rp = toml_get(rule, "path");
                if ((rn.type == TOML_STRING && !strcmp(rn.u.s, name)) ||
                    (rp.type == TOML_STRING && path_matches(c, rp.u.s, path)))
                    apply_enabled(toml_get(rule, "enabled"), &item_enabled);
            }
        } else {
            const char *section = !strcmp(kind, "plugin") ? "plugins" : "mcp_servers";
            toml_datum_t group = toml_get(root, section);
            apply_enabled(toml_get(group, "enabled"), &group_enabled);
            if (!strcmp(kind, "plugin")) {
                char bare[257];
                size_t n = strcspn(name, "@");
                if (n < sizeof(bare)) {
                    memcpy(bare, name, n);
                    bare[n] = 0;
                    apply_enabled(toml_get(toml_get(group, bare), "enabled"), &item_enabled);
                }
            }
            apply_enabled(toml_get(toml_get(group, name), "enabled"), &item_enabled);
            if (!strcmp(kind, "mcp"))
                apply_enabled(toml_seek(root, "mcp.enabled"), &bundled_enabled);
        }
    }
    return !group_enabled || !item_enabled || !bundled_enabled;
}

yyjson_mut_val *ext_toml_json(yyjson_mut_doc *doc, toml_datum_t value, unsigned depth) {
    if (depth > 20)
        return NULL;
    switch (value.type) {
    case TOML_STRING:
        return yyjson_mut_strncpy(doc, value.u.str.ptr, (size_t)value.u.str.len);
    case TOML_INT64:
        return yyjson_mut_sint(doc, value.u.int64);
    case TOML_FP64:
        return yyjson_mut_real(doc, value.u.fp64);
    case TOML_BOOLEAN:
        return yyjson_mut_bool(doc, value.u.boolean);
    case TOML_TABLE: {
        yyjson_mut_val *obj = yyjson_mut_obj(doc);
        for (int i = 0; i < value.u.tab.size; ++i) {
            yyjson_mut_val *item = ext_toml_json(doc, value.u.tab.value[i], depth + 1);
            if (!item || !yyjson_mut_obj_add_val(doc, obj, value.u.tab.key[i], item))
                return NULL;
        }
        return obj;
    }
    case TOML_ARRAY: {
        yyjson_mut_val *arr = yyjson_mut_arr(doc);
        for (int i = 0; i < value.u.arr.size; ++i) {
            yyjson_mut_val *item = ext_toml_json(doc, value.u.arr.elem[i], depth + 1);
            if (!item || !yyjson_mut_arr_append(arr, item))
                return NULL;
        }
        return arr;
    }
    default:
        return NULL;
    }
}

ah_code ext_servers_load(ext_catalog *c, ah_error *error) {
    for (size_t layer = 0; layer < 2; ++layer) {
        toml_datum_t servers = toml_get(c->config[layer].toptab, "mcp_servers");
        if (servers.type == TOML_UNKNOWN)
            continue;
        if (servers.type != TOML_TABLE)
            return ah_fail(error, AH_INVALID, "mcp_servers must be a TOML table");
        for (int i = 0; i < servers.u.tab.size; ++i) {
            const char *name = servers.u.tab.key[i];
            if (!strcmp(name, "enabled"))
                continue;
            toml_datum_t config = servers.u.tab.value[i];
            toml_datum_t project = toml_get(toml_get(c->config[1].toptab, "mcp_servers"), name);
            if (!layer && project.type != TOML_UNKNOWN)
                continue;
            toml_result_t merged = {0};
            toml_datum_t user = toml_get(toml_get(c->config[0].toptab, "mcp_servers"), name);
            if (layer && user.type == TOML_TABLE && config.type == TOML_TABLE) {
                toml_result_t base = {.ok = true, .toptab = user};
                toml_result_t override = {.ok = true, .toptab = config};
                merged = toml_merge(&base, &override);
                if (!merged.ok) {
                    toml_free(merged);
                    return ah_fail(error, AH_INVALID, "Cannot merge MCP configuration");
                }
                config = merged.toptab;
            }
            yyjson_mut_doc *doc = ah_mut_doc_new();
            yyjson_mut_val *value = ext_toml_json(doc, config, 0);
            yyjson_mut_doc_set_root(doc, value);
            char *json = value ? ah_mut_write(doc) : NULL;
            ah_mut_doc_free(doc);
            toml_free(merged);
            if (!json)
                return ah_fail(error, AH_INVALID, "Invalid MCP server configuration");
            yyjson_doc *parsed = yyjson_read(json, strlen(json), 0);
            free(json);
            ah_code rc =
                parsed ? ext_server_add(c, name, NULL, yyjson_doc_get_root(parsed), NULL, error)
                       : ah_fail(error, AH_INVALID, "Invalid MCP server configuration");
            yyjson_doc_free(parsed);
            if (rc != AH_OK)
                return rc;
        }
    }
    return AH_OK;
}
