#include "catalog_private.h"
#include <ctype.h>

ah_code ext_catalog_open(const ah_extensions_config *options, ext_catalog **out, ah_error *error) {
    if (!options || !out)
        return ah_fail(error, AH_INVALID, "Missing extension configuration");
    *out = NULL;
    ext_catalog *c = calloc(1, sizeof(*c));
    if (!c)
        return ah_fail(error, AH_IO, "Out of memory");
    if (pthread_mutex_init(&c->mutex, NULL)) {
        free(c);
        return ah_fail(error, AH_IO, "Cannot initialize extension catalog mutex");
    }
    c->options = *options;
    if (!c->options.max_catalog_entries)
        c->options.max_catalog_entries = 512;
    if (!c->options.max_catalog_bytes)
        c->options.max_catalog_bytes = 2u * 1024u * 1024u;
    if (!c->options.max_result_bytes)
        c->options.max_result_bytes = 64u * 1024u;
    if (c->options.max_catalog_entries > 10000 ||
        c->options.max_catalog_bytes > 64u * 1024u * 1024u) {
        pthread_mutex_destroy(&c->mutex);
        free(c);
        return ah_fail(error, AH_LIMIT, "Extension catalog limits exceed hard limits");
    }
    const char *home = options->codex_home;
    if (!home || !*home)
        home = getenv("CODEX_HOME");
    const char *user = getenv("HOME");
    c->user_home = ah_strdup(user ? user : "");
    c->home = home && *home ? ah_strdup(home) : ext_join(user ? user : "", ".codex");
    c->workspace = ah_strdup(options->workspace ? options->workspace : "");
    c->entries = calloc(c->options.max_catalog_entries, sizeof(*c->entries));
    if (!c->user_home || !c->home || !c->workspace || !c->entries) {
        ext_catalog_close(c);
        return ah_fail(error, AH_IO, "Out of memory");
    }
    c->retained = sizeof(*c) + c->options.max_catalog_entries * sizeof(ext_entry) +
                  strlen(c->user_home) + strlen(c->home) + strlen(c->workspace) + 3;
    if (c->retained > c->options.max_catalog_bytes) {
        ext_catalog_close(c);
        return ah_fail(error, AH_LIMIT, "Extension metadata budget is too small");
    }
    *out = c;
    return AH_OK;
}

static bool contains(const char *text, const char *query) {
    if (!query || !*query)
        return true;
    for (; *text; ++text) {
        const char *a = text, *b = query;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            ++a;
            ++b;
        }
        if (!*b)
            return true;
    }
    return false;
}

static bool matches(const ext_entry *e, const char *kind, const char *query, const char *plugin) {
    if (strcmp(kind, "all") && strcmp(kind, e->kind))
        return false;
    if (plugin && *plugin && strcmp(plugin, e->plugin) && strcmp(plugin, e->id))
        return false;
    return contains(e->name, query) || contains(e->description, query) || contains(e->id, query);
}

static int compare_entries(const void *a, const void *b) {
    return strcmp((*(ext_entry *const *)a)->id, (*(ext_entry *const *)b)->id);
}

static ah_code discover(ext_catalog *c, const char *kind, const char *plugin, ah_error *error) {
    if (c->failure.code != AH_OK) {
        if (error)
            *error = c->failure;
        return c->failure.code;
    }
    bool all = !strcmp(kind, "all");
    bool skills = all || !strcmp(kind, "skill"), servers = all || !strcmp(kind, "mcp");
    bool plugins = all || !strcmp(kind, "plugin") || (plugin && *plugin);
    if (!all && strcmp(kind, "skill") && strcmp(kind, "mcp") && strcmp(kind, "plugin"))
        return ah_fail(error, AH_INVALID, "Unknown extension kind");
    if ((skills && c->options.enable_skills) || (servers && c->options.enable_mcp) ||
        (plugins && c->options.enable_plugins)) {
        ah_code rc = ext_config_load(c, &c->failure);
        if (rc != AH_OK) {
            if (error)
                *error = c->failure;
            return rc;
        }
    }
    ah_code rc = AH_OK;
    if (skills && c->options.enable_skills && !c->skills_loaded && (!plugin || !*plugin)) {
        rc = ext_skills_load(c, &c->failure);
        if (rc == AH_OK)
            c->skills_loaded = true;
    }
    if (rc == AH_OK && servers && c->options.enable_mcp && !c->servers_loaded &&
        (!plugin || !*plugin)) {
        rc = ext_servers_load(c, &c->failure);
        if (rc == AH_OK)
            c->servers_loaded = true;
    }
    if (rc == AH_OK && plugins && c->options.enable_plugins && !c->plugins_loaded) {
        rc = ext_plugins_load(c, &c->failure);
        if (rc == AH_OK)
            c->plugins_loaded = true;
    }
    if (rc != AH_OK && error)
        *error = c->failure;
    if (rc == AH_OK && plugin && *plugin) {
        if (!c->options.enable_plugins)
            return ah_fail(error, AH_INVALID, "Plugin support is disabled");
        rc = ext_plugin_expand(c, plugin, error);
    }
    return rc;
}

static ah_code catalog_query(ext_catalog *c, const char *kind, const char *query,
                             const char *plugin, size_t offset, size_t limit, char **json,
                             ah_error *error) {
    if (!c || !kind || !json)
        return ah_fail(error, AH_INVALID, "Missing catalog query");
    *json = NULL;
    ah_code rc = discover(c, kind, plugin, error);
    if (rc != AH_OK)
        return rc;
    if (!limit)
        limit = 20;
    if (limit > 100)
        limit = 100;
    ext_entry **items = calloc(c->count + 1, sizeof(*items));
    if (!items)
        return ah_fail(error, AH_IO, "Out of memory");
    size_t count = 0;
    for (size_t i = 0; i < c->count; ++i)
        if (matches(&c->entries[i], kind, query, plugin))
            items[count++] = &c->entries[i];
    qsort(items, count, sizeof(*items), compare_entries);
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(doc), *array = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_val(doc, root, "items", array);
    size_t end =
        offset < count ? offset + (limit < count - offset ? limit : count - offset) : count;
    for (size_t i = offset; i < end; ++i) {
        ext_entry *e = items[i];
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_arr_append(array, item);
        yyjson_mut_obj_add_strcpy(doc, item, "id", e->id);
        yyjson_mut_obj_add_strcpy(doc, item, "kind", e->kind);
        yyjson_mut_obj_add_strcpy(doc, item, "name", e->name);
        yyjson_mut_obj_add_strcpy(doc, item, "description", e->description);
        yyjson_mut_obj_add_bool(doc, item, "enabled", e->enabled);
        yyjson_mut_obj_add_bool(doc, item, "available", e->available);
        if (*e->plugin)
            yyjson_mut_obj_add_strcpy(doc, item, "plugin", e->plugin);
        if (*e->reason)
            yyjson_mut_obj_add_strcpy(doc, item, "reason", e->reason);
        if (!strcmp(e->kind, "plugin"))
            yyjson_mut_obj_add_bool(doc, item, "expanded", e->expanded);
    }
    yyjson_mut_obj_add_uint(doc, root, "total", count);
    if (end < count)
        yyjson_mut_obj_add_uint(doc, root, "next_offset", end);
    else
        yyjson_mut_obj_add_null(doc, root, "next_offset");
    *json = ah_mut_write(doc);
    ah_mut_doc_free(doc);
    free(items);
    if (!*json)
        return ah_fail(error, AH_IO, "Cannot serialize extension catalog");
    if (strlen(*json) > c->options.max_result_bytes) {
        free(*json);
        *json = NULL;
        return ah_fail(error, AH_LIMIT, "Catalog response too large; request a smaller page");
    }
    return AH_OK;
}

static ah_code lookup(ext_catalog *c, const char *id, const char *kind, ext_entry **entry,
                      ah_error *error) {
    *entry = ext_find(c, id);
    if (!*entry) {
        ah_code rc = discover(c, kind, NULL, error);
        if (rc != AH_OK)
            return rc;
        *entry = ext_find(c, id);
    }
    if (!*entry || strcmp((*entry)->kind, kind))
        return ah_fail(error, AH_NOT_FOUND, "Unknown extension ID");
    if (!(*entry)->enabled || !(*entry)->available)
        return ah_fail(error, AH_INVALID, "Extension unavailable: %s", (*entry)->reason);
    return AH_OK;
}

static ah_code catalog_skill(ext_catalog *c, const char *id, char **path, ah_error *error) {
    if (!c || !id || !path)
        return ah_fail(error, AH_INVALID, "Missing skill selection");
    *path = NULL;
    if (!c->options.enable_skills)
        return ah_fail(error, AH_INVALID, "Skill support is disabled");
    ext_entry *entry;
    ah_code rc = lookup(c, id, "skill", &entry, error);
    if (rc != AH_OK)
        return rc;
    *path = realpath(entry->path, NULL);
    if (!*path)
        return ah_fail(error, AH_IO, "Skill path is no longer available");
    ext_entry *plugin = *entry->plugin ? ext_find(c, entry->plugin) : NULL;
    if (plugin && !ext_inside(plugin->path, *path)) {
        free(*path);
        *path = NULL;
        return ah_fail(error, AH_INVALID, "Plugin skill path escapes plugin root");
    }
    return AH_OK;
}

static ah_code catalog_server(ext_catalog *c, const char *id, char **json, ah_error *error) {
    if (!c || !id || !json)
        return ah_fail(error, AH_INVALID, "Missing server selection");
    *json = NULL;
    if (!c->options.enable_mcp)
        return ah_fail(error, AH_INVALID, "MCP support is disabled");
    ext_entry *entry;
    ah_code rc = lookup(c, id, "mcp", &entry, error);
    if (rc != AH_OK)
        return rc;
    *json = ah_strdup(entry->config);
    return *json ? AH_OK : ah_fail(error, AH_IO, "Out of memory");
}

ah_code ext_catalog_query(ext_catalog *c, const char *kind, const char *query, const char *plugin,
                          size_t offset, size_t limit, char **json, ah_error *error) {
    if (!c)
        return ah_fail(error, AH_INVALID, "Missing catalog");
    pthread_mutex_lock(&c->mutex);
    ah_code code = catalog_query(c, kind, query, plugin, offset, limit, json, error);
    pthread_mutex_unlock(&c->mutex);
    return code;
}

ah_code ext_catalog_skill(ext_catalog *c, const char *id, char **path, ah_error *error) {
    if (!c)
        return ah_fail(error, AH_INVALID, "Missing catalog");
    pthread_mutex_lock(&c->mutex);
    ah_code code = catalog_skill(c, id, path, error);
    pthread_mutex_unlock(&c->mutex);
    return code;
}

ah_code ext_catalog_server(ext_catalog *c, const char *id, char **json, ah_error *error) {
    if (!c)
        return ah_fail(error, AH_INVALID, "Missing catalog");
    pthread_mutex_lock(&c->mutex);
    ah_code code = catalog_server(c, id, json, error);
    pthread_mutex_unlock(&c->mutex);
    return code;
}
