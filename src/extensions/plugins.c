#include "catalog_private.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

static ah_code plugin_path(const char *root, const char *relative, char **out, ah_error *error) {
    *out = NULL;
    if (!relative || relative[0] == '/' || strstr(relative, "${"))
        return ah_fail(error, AH_INVALID, "Plugin resource path must be relative");
    char *joined = ext_join(root, relative);
    if (!joined)
        return ah_fail(error, AH_IO, "Out of memory");
    char *resolved = realpath(joined, NULL);
    free(joined);
    if (!resolved)
        return errno == ENOENT || errno == ENOTDIR
                   ? AH_NOT_FOUND
                   : ah_fail(error, AH_IO, "Cannot resolve plugin resource");
    if (!ext_inside(root, resolved)) {
        free(resolved);
        return ah_fail(error, AH_INVALID, "Plugin resource escapes plugin root");
    }
    *out = resolved;
    return AH_OK;
}

static ah_code plugin_json(ext_catalog *c, const char *root, const char *relative, yyjson_doc **doc,
                           ah_error *error) {
    *doc = NULL;
    char *path = NULL;
    ah_code rc = plugin_path(root, relative, &path, error);
    if (rc == AH_OK)
        rc = ext_json_file(c, path, doc, error);
    free(path);
    return rc;
}

static const char *json_string(yyjson_val *object, const char *key, const char *fallback) {
    const char *value = yyjson_get_str(yyjson_obj_get(object, key));
    return value ? value : fallback;
}

static ah_code unavailable_plugin(ext_catalog *c, const char *root, const char *market,
                                  const char *directory, ah_error *error) {
    size_t size = strlen(directory) + strlen(market) + 2;
    char *id = malloc(size);
    if (!id)
        return ah_fail(error, AH_IO, "Out of memory");
    snprintf(id, size, "%s@%s", directory, market);
    ext_entry entry = {.id = id,
                       .kind = "plugin",
                       .name = (char *)directory,
                       .path = (char *)root,
                       .enabled = !ext_disabled(c, "plugin", id, root),
                       .reason = "plugin manifest missing, malformed, or outside installation root",
                       .expansion_failure = AH_INVALID};
    ah_code rc = ext_add(c, &entry, error);
    free(id);
    return rc;
}

static ah_code add_plugin(ext_catalog *c, const char *root, const char *market,
                          const char *directory, ah_error *error) {
    yyjson_doc *doc = NULL, *overlay = NULL;
    ah_code rc = plugin_json(c, root, "plugin.json", &doc, error);
    bool portable = rc == AH_OK;
    if (rc == AH_NOT_FOUND)
        rc = plugin_json(c, root, ".codex-plugin/plugin.json", &doc, error);
    if (rc == AH_INVALID || rc == AH_NOT_FOUND) {
        ah_error_clear(error);
        return unavailable_plugin(c, root, market, directory, error);
    }
    if (rc != AH_OK)
        return rc;
    yyjson_val *manifest = yyjson_doc_get_root(doc);
    const char *name = json_string(manifest, "name", NULL);
    if (!name || !*name || strlen(name) > 256 || strchr(name, '@') || strchr(name, '/')) {
        yyjson_doc_free(doc);
        return unavailable_plugin(c, root, market, directory, error);
    }
    yyjson_val *settings = manifest;
    if (portable) {
        settings = yyjson_obj_get(yyjson_obj_get(manifest, "extensions"), "com.openai");
        if (!settings) {
            rc = plugin_json(c, root, ".codex-plugin/plugin.json", &overlay, error);
            if (rc == AH_INVALID) {
                yyjson_doc_free(doc);
                ah_error_clear(error);
                return unavailable_plugin(c, root, market, directory, error);
            }
            if (rc != AH_OK && rc != AH_NOT_FOUND) {
                yyjson_doc_free(doc);
                return rc;
            }
            settings = overlay ? yyjson_doc_get_root(overlay) : NULL;
        }
    }
    size_t id_size = strlen(directory) + strlen(market) + 2;
    char *id = malloc(id_size);
    if (id)
        snprintf(id, id_size, "%s@%s", directory, market);
    yyjson_mut_doc *normalized = ah_mut_doc_new();
    yyjson_mut_val *paths = yyjson_mut_obj(normalized);
    yyjson_mut_doc_set_root(normalized, paths);
    yyjson_mut_obj_add_bool(normalized, paths, "portable", portable);
    const char *fields[] = {"skills", "mcpServers", "apps", "hooks"};
    for (size_t i = 0; i < 4; ++i) {
        yyjson_val *value = yyjson_obj_get(settings, fields[i]);
        if (value)
            yyjson_mut_obj_add_val(normalized, paths, fields[i],
                                   yyjson_val_mut_copy(normalized, value));
    }
    char *json = ah_mut_write(normalized);
    ah_mut_doc_free(normalized);
    bool enabled = id && !ext_disabled(c, "plugin", id, root);
    bool hosted = yyjson_obj_get(settings, "apps") || yyjson_obj_get(settings, "hooks");
    const char *reason = !enabled ? "disabled by configuration"
                         : hosted ? "hosted app connectors or lifecycle hooks unavailable; "
                                    "portable components can be expanded"
                                  : "";
    ext_entry entry = {.id = id,
                       .kind = "plugin",
                       .name = (char *)name,
                       .description = (char *)json_string(manifest, "description", ""),
                       .path = (char *)root,
                       .config = json,
                       .enabled = enabled,
                       .available = !hosted,
                       .reason = (char *)reason};
    rc = id && json ? ext_add(c, &entry, error) : ah_fail(error, AH_IO, "Out of memory");
    free(id);
    free(json);
    yyjson_doc_free(overlay);
    yyjson_doc_free(doc);
    return rc;
}

/* Numeric runs make 1.10 newer than 1.9; remaining suffixes break ties stably. */
static int version_compare(const char *a, const char *b) {
    const char *start_a = a, *start_b = b;
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            const char *ae = a, *be = b;
            while (*a == '0')
                ++a;
            while (*b == '0')
                ++b;
            ae = a;
            be = b;
            while (isdigit((unsigned char)*ae))
                ++ae;
            while (isdigit((unsigned char)*be))
                ++be;
            size_t an = (size_t)(ae - a), bn = (size_t)(be - b);
            if (an != bn)
                return an < bn ? -1 : 1;
            int order = strncmp(a, b, an);
            if (order)
                return order;
            a = ae;
            b = be;
        } else {
            if (*a != *b)
                return (unsigned char)*a - (unsigned char)*b;
            ++a;
            ++b;
        }
    }
    if (*a != *b)
        return (unsigned char)*a - (unsigned char)*b;
    return strcmp(start_a, start_b);
}

static ah_code scan_versions(ext_catalog *c, const char *cache, const char *market,
                             const char *name, ah_error *error) {
    char *market_path = ext_join(cache, market),
         *path = market_path ? ext_join(market_path, name) : NULL;
    free(market_path);
    if (!path)
        return ah_fail(error, AH_IO, "Out of memory");
    char **versions = NULL;
    size_t count = 0;
    ah_code rc = ext_children(c, path, &versions, &count, error);
    char *selected = NULL;
    const char *version = NULL;
    for (size_t i = 0; i < count && rc == AH_OK; ++i) {
        char *candidate = ext_join(path, versions[i]);
        struct stat st;
        if (!candidate) {
            rc = ah_fail(error, AH_IO, "Out of memory");
            break;
        }
        if (!stat(candidate, &st) && S_ISDIR(st.st_mode) &&
            (!version || version_compare(versions[i], version) > 0)) {
            free(selected);
            selected = candidate;
            version = versions[i];
        } else
            free(candidate);
    }
    if (rc == AH_OK && selected) {
        char *root = realpath(selected, NULL);
        /* Installed version symlinks may point into the Codex application's
         * bundled resources. That canonical installation is the containment root. */
        if (!root)
            rc = ah_fail(error, AH_IO, "Cannot resolve installed plugin root");
        else
            rc = add_plugin(c, root, market, name, error);
        free(root);
    }
    free(selected);
    free(path);
    ext_names_free(versions, count);
    return rc;
}

ah_code ext_plugins_load(ext_catalog *c, ah_error *error) {
    char *path = ext_join(c->home, "plugins/cache");
    char *cache = path ? realpath(path, NULL) : NULL;
    free(path);
    if (!cache)
        return errno == ENOENT || errno == ENOTDIR
                   ? AH_OK
                   : ah_fail(error, AH_IO, "Cannot resolve plugin cache");
    char **markets = NULL;
    size_t count = 0;
    ah_code rc = ext_children(c, cache, &markets, &count, error);
    for (size_t i = 0; i < count && rc == AH_OK; ++i) {
        if (markets[i][0] == '.')
            continue;
        char *market = ext_join(cache, markets[i]);
        char **names = NULL;
        size_t n = 0;
        rc = market ? ext_children(c, market, &names, &n, error)
                    : ah_fail(error, AH_IO, "Out of memory");
        for (size_t j = 0; j < n && rc == AH_OK; ++j)
            if (names[j][0] != '.')
                rc = scan_versions(c, cache, markets[i], names[j], error);
        free(market);
        ext_names_free(names, n);
    }
    free(cache);
    ext_names_free(markets, count);
    return rc;
}

static ah_code plugin_skills(ext_catalog *c, ext_entry *plugin, yyjson_val *paths,
                             ah_error *error) {
    if (!c->options.enable_skills)
        return AH_OK;
    yyjson_val *skills = yyjson_obj_get(paths, "skills");
    bool portable = yyjson_is_true(yyjson_obj_get(paths, "portable"));
    size_t count = yyjson_is_arr(skills) ? yyjson_arr_size(skills) : 1;
    for (size_t i = 0; i < count; ++i) {
        const char *relative =
            portable || !skills
                ? "skills"
                : yyjson_get_str(yyjson_is_arr(skills) ? yyjson_arr_get(skills, i) : skills);
        char *path = NULL;
        ah_code rc = plugin_path(plugin->path, relative, &path, error);
        if (rc == AH_NOT_FOUND)
            continue;
        if (rc == AH_OK)
            rc = ext_skills_walk(c, path, plugin->id, plugin->path, 0, error);
        free(path);
        if (rc != AH_OK)
            return rc;
        if (portable)
            break;
    }
    return AH_OK;
}

static ah_code plugin_servers(ext_catalog *c, ext_entry *plugin, yyjson_val *paths,
                              ah_error *error) {
    if (!c->options.enable_mcp)
        return AH_OK;
    yyjson_val *servers = yyjson_obj_get(paths, "mcpServers");
    bool portable = yyjson_is_true(yyjson_obj_get(paths, "portable"));
    yyjson_doc *doc = NULL;
    if (portable || !yyjson_is_obj(servers)) {
        const char *relative = portable  ? "mcp.json"
                               : servers ? yyjson_get_str(servers)
                                         : ".mcp.json";
        ah_code rc = plugin_json(c, plugin->path, relative, &doc, error);
        if (rc == AH_NOT_FOUND)
            return AH_OK;
        if (rc != AH_OK)
            return rc;
        servers = yyjson_obj_get(yyjson_doc_get_root(doc), "mcpServers");
        if (!servers && !portable)
            servers = yyjson_doc_get_root(doc);
    }
    if (!yyjson_is_obj(servers)) {
        yyjson_doc_free(doc);
        return ah_fail(error, AH_INVALID, "Plugin MCP configuration requires mcpServers object");
    }
    ah_code rc = AH_OK;
    yyjson_obj_iter it = yyjson_obj_iter_with(servers);
    yyjson_val *key;
    while (rc == AH_OK && (key = yyjson_obj_iter_next(&it)))
        rc = ext_server_add(c, yyjson_get_str(key), plugin->id, yyjson_obj_iter_get_val(key),
                            plugin->path, error);
    yyjson_doc_free(doc);
    return rc;
}

ah_code ext_plugin_expand(ext_catalog *c, const char *id, ah_error *error) {
    ext_entry *plugin = ext_find(c, id);
    if (!plugin || strcmp(plugin->kind, "plugin"))
        return ah_fail(error, AH_NOT_FOUND, "Unknown plugin ID");
    if (!plugin->enabled)
        return ah_fail(error, AH_INVALID, "Plugin is disabled by configuration");
    if (plugin->expansion_failure != AH_OK)
        return ah_fail(error, plugin->expansion_failure, "Plugin components are unavailable");
    if (plugin->expanded)
        return AH_OK;
    yyjson_doc *doc = yyjson_read(plugin->config, strlen(plugin->config), 0);
    if (!doc)
        return ah_fail(error, AH_INVALID, "Invalid plugin metadata");
    yyjson_val *paths = yyjson_doc_get_root(doc);
    size_t first_child = c->count;
    ah_code rc = plugin_skills(c, plugin, paths, error);
    if (rc == AH_OK)
        rc = plugin_servers(c, plugin, paths, error);
    if (rc == AH_OK)
        plugin->expanded = true;
    else {
        plugin->available = false;
        plugin->expansion_failure = rc;
        char *reason = ah_strdup(
            "plugin component metadata is malformed, unsupported, or outside installation root");
        if (reason && strlen(reason) <= ext_remaining(c) + strlen(plugin->reason)) {
            c->retained -= strlen(plugin->reason) + 1;
            c->retained += strlen(reason) + 1;
            free(plugin->reason);
            plugin->reason = reason;
        } else
            free(reason);
        for (size_t i = first_child; i < c->count; ++i)
            c->entries[i].available = false;
    }
    yyjson_doc_free(doc);
    return rc;
}
