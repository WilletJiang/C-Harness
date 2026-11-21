#include "extensions_internal.h"
#include <stdio.h>
#include <errno.h>
#include <time.h>

static ah_code server_get(ah_extensions *x, const char *id, ext_server **out, ah_error *error) {
    *out = NULL;
    if (!id || !*id)
        return ah_fail(error, AH_INVALID, "server ID required");
    for (ext_server *s = x->servers; s; s = s->next)
        if (!strcmp(s->id, id)) {
            *out = s;
            return AH_OK;
        }
    if (x->server_count >= 32)
        return ah_fail(error, AH_LIMIT, "maximum 32 opened MCP servers per engine");
    char *json = NULL;
    ah_code rc = ext_catalog_server(x->catalog, id, &json, error);
    if (rc != AH_OK)
        return rc;
    ext_server *s = calloc(1, sizeof(*s));
    if (!s) {
        free(json);
        return ah_fail(error, AH_IO, "out of memory");
    }
    if (pthread_mutex_init(&s->request_mutex, NULL)) {
        free(json);
        free(s);
        return ah_fail(error, AH_IO, "cannot initialize MCP server mutex");
    }
    s->id = ah_strdup(id);
    s->config = yyjson_read(json, strlen(json), 0);
    if (!s->id || !s->config)
        rc = ah_fail(error, AH_IO, "cannot retain server configuration");
    if (rc == AH_OK)
        rc = mcp_client_open(json, x->config.max_result_bytes, x->config.allow_http_loopback,
                             &s->client, error);
    free(json);
    if (rc != AH_OK) {
        pthread_mutex_destroy(&s->request_mutex);
        free(s->id);
        yyjson_doc_free(s->config);
        free(s);
        return rc;
    }
    s->next = x->servers;
    x->servers = s;
    x->server_count++;
    *out = s;
    return AH_OK;
}
ah_code ext_server_get(ah_extensions *x, const char *id, ext_server **out, ah_error *error) {
    pthread_mutex_lock(&x->mutex);
    ah_code code = server_get(x, id, out, error);
    pthread_mutex_unlock(&x->mutex);
    return code;
}

/* Only worker callbacks use this request lock. Waiting honors cancellation;
 * unrelated servers retain independent transport and cache ownership. */
static ah_code acquire(ext_server *server, const ah_tool_call *call, ah_error *error) {
    for (;;) {
        if (call->cancelled && call->cancelled(call->cancel_context))
            return ah_fail(error, AH_CANCELLED, "cancelled while waiting for MCP server");
        if (call->deadline_ms && ah_now_ms() >= call->deadline_ms)
            return ah_fail(error, AH_LIMIT, "deadline while waiting for MCP server");
        int code = pthread_mutex_trylock(&server->request_mutex);
        if (!code)
            return AH_OK;
        if (code != EBUSY)
            return ah_fail(error, AH_IO, "cannot acquire MCP server");
        struct timespec delay = {.tv_nsec = 10000000};
        nanosleep(&delay, NULL);
    }
}

static bool contains(yyjson_val *array, const char *name) {
    if (!yyjson_is_arr(array))
        return false;
    size_t index, count;
    yyjson_val *value;
    yyjson_arr_foreach(array, index, count,
                       value) if (yyjson_is_str(value) &&
                                  !strcmp(yyjson_get_str(value), name)) return true;
    return false;
}
bool ext_tool_allowed(ext_server *s, const char *name) {
    yyjson_val *root = yyjson_doc_get_root(s->config);
    yyjson_val *enabled = yyjson_obj_get(root, "enabled_tools"),
               *disabled = yyjson_obj_get(root, "disabled_tools");
    return name && *name && (!enabled || contains(enabled, name)) && !contains(disabled, name);
}
static char *parameters(const char *key, const char *value, yyjson_val *arguments) {
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    if (value)
        yyjson_mut_obj_add_strcpy(doc, root, key, value);
    if (arguments)
        yyjson_mut_obj_add_val(doc, root, "arguments", yyjson_val_mut_copy(doc, arguments));
    char *json = ah_mut_write(doc);
    ah_mut_doc_free(doc);
    return json;
}
static ah_code list_page(ah_extensions *x, ext_server *s, const char *kind, const char *cursor,
                         bool refresh, const ah_tool_call *call, yyjson_val **root,
                         ah_error *error) {
    yyjson_doc **cache = !strcmp(kind, "tools")       ? &s->tools
                         : !strcmp(kind, "resources") ? &s->resources
                         : !strcmp(kind, "templates") ? &s->templates
                                                      : &s->prompts;
    char **stored_cursor = !strcmp(kind, "tools")       ? &s->tools_cursor
                           : !strcmp(kind, "resources") ? &s->resources_cursor
                           : !strcmp(kind, "templates") ? &s->templates_cursor
                                                        : &s->prompts_cursor;
    const char *page_cursor = cursor ? cursor : "";
    if (!refresh && *cache && *stored_cursor && !strcmp(*stored_cursor, page_cursor)) {
        *root = yyjson_doc_get_root(*cache);
        return AH_OK;
    }
    char method[32];
    snprintf(method, sizeof(method), "%s/list",
             !strcmp(kind, "templates") ? "resources/templates" : kind);
    const char *key = !strcmp(kind, "templates") ? "resourceTemplates" : kind;
    char *params = parameters("cursor", *page_cursor ? page_cursor : NULL, NULL), *json = NULL;
    bool sent = false;
    ah_code rc = params ? mcp_client_request(s->client, method, params, call, &json, &sent, error)
                        : ah_fail(error, AH_IO, "out of memory");
    free(params);
    if (rc != AH_OK)
        return rc;
    yyjson_doc *parsed = yyjson_read(json, strlen(json), 0);
    yyjson_val *value = yyjson_doc_get_root(parsed);
    if (!parsed || !yyjson_is_arr(yyjson_obj_get(value, key))) {
        free(json);
        yyjson_doc_free(parsed);
        return ah_fail(error, AH_PROTOCOL, "MCP server did not return a supported list");
    }
    size_t old_bytes = (*cache ? yyjson_doc_get_read_size(*cache) : 0) +
                       (*stored_cursor ? strlen(*stored_cursor) + 1 : 0);
    size_t bytes = strlen(json) + strlen(page_cursor) + 1;
    free(json);
    char *copy = ah_strdup(page_cursor);
    if (!copy) {
        yyjson_doc_free(parsed);
        return ah_fail(error, AH_IO, "cannot retain MCP list cursor");
    }
    pthread_mutex_lock(&x->mutex);
    size_t retained = x->cache_bytes - old_bytes;
    bool fits =
        retained <= x->config.max_catalog_bytes && bytes <= x->config.max_catalog_bytes - retained;
    if (fits)
        x->cache_bytes = retained + bytes;
    pthread_mutex_unlock(&x->mutex);
    if (!fits) {
        free(copy);
        yyjson_doc_free(parsed);
        return ah_fail(error, AH_LIMIT, "MCP metadata cache budget exceeded");
    }
    yyjson_doc_free(*cache);
    free(*stored_cursor);
    *cache = parsed;
    *stored_cursor = copy;
    *root = value;
    return AH_OK;
}
static ah_code metadata(ext_server *s, const char *kind, yyjson_val *source, yyjson_val *args,
                        ah_tool_result *result, ah_error *error) {
    const char *query = ext_string(args, "query");
    size_t offset = ext_number(args, "offset", 0), limit = ext_number(args, "limit", 12);
    size_t matches = 0, emitted = 0, index, count;
    yyjson_val *item;
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(doc), *items = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "server", s->id);
    yyjson_mut_obj_add_val(doc, root, "items", items);
    const char *key = !strcmp(kind, "templates") ? "resourceTemplates" : kind;
    yyjson_arr_foreach(yyjson_obj_get(source, key), index, count, item) {
        const char *name = ext_string(item, "name"), *description = ext_string(item, "description");
        if ((!strcmp(kind, "tools") && !ext_tool_allowed(s, name)) ||
            (!ext_match(name, query) && !ext_match(description, query)))
            continue;
        if (matches++ < offset || emitted >= limit)
            continue;
        yyjson_mut_val *entry = yyjson_mut_obj(doc);
        const char *keys[] = {"name", "title", "description", "uri", "uriTemplate", "mimeType"};
        for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); ++i) {
            const char *text = ext_string(item, keys[i]);
            if (text) {
                size_t bytes = strlen(text), cap = !strcmp(keys[i], "description") ? 512
                                                   : !strcmp(keys[i], "title")     ? 2048
                                                                                   : bytes;
                if (bytes > cap) {
                    bytes = cap;
                    while (bytes && ((unsigned char)text[bytes] & 0xc0) == 0x80)
                        --bytes;
                }
                yyjson_mut_obj_add_strncpy(doc, entry, keys[i], text, bytes);
            }
        }
        yyjson_mut_arr_append(items, entry);
        emitted++;
    }
    yyjson_mut_obj_add_uint(doc, root, "total_in_page", matches);
    yyjson_mut_obj_add_uint(doc, root, "next_offset", offset + emitted);
    yyjson_mut_obj_add_bool(doc, root, "more", offset + emitted < matches);
    const char *next = ext_string(source, "nextCursor");
    if (next)
        yyjson_mut_obj_add_strcpy(doc, root, "next_cursor", next);
    ah_code rc = ext_result(doc, result, error);
    ah_mut_doc_free(doc);
    return rc;
}
static yyjson_val *find_tool(yyjson_val *root, const char *name) {
    size_t index, count;
    yyjson_val *item;
    yyjson_arr_foreach(yyjson_obj_get(root, "tools"), index, count, item) {
        const char *candidate = ext_string(item, "name");
        if (name && candidate && !strcmp(name, candidate))
            return item;
    }
    return NULL;
}
ah_code ext_mcp(void *context, const ah_tool_call *call, ah_tool_result *result, ah_error *error) {
    ah_extensions *x = context;
    yyjson_doc *doc = yyjson_read(call->arguments_json, strlen(call->arguments_json), 0);
    yyjson_val *args = yyjson_doc_get_root(doc), *page = NULL;
    const char *action = ext_string(args, "action"), *name = ext_string(args, "name");
    ext_server *s = NULL;
    ah_code rc = ext_server_get(x, ext_string(args, "server"), &s, error);
    if (rc == AH_OK)
        rc = acquire(s, call, error);
    bool acquired = rc == AH_OK;
    if (rc == AH_OK && (!strcmp(action, "read") || !strcmp(action, "get"))) {
        bool read = !strcmp(action, "read"), sent = false;
        const char *value = read ? ext_string(args, "uri") : name;
        char *params = value ? parameters(read ? "uri" : "name", value,
                                          read ? NULL : yyjson_obj_get(args, "arguments"))
                             : NULL;
        rc = params ? mcp_client_request(s->client, read ? "resources/read" : "prompts/get", params,
                                         call, &result->json, &sent, error)
                    : ah_fail(error, AH_INVALID, "resource uri or prompt name required");
        free(params);
    } else if (rc == AH_OK) {
        bool describe = !strcmp(action, "describe");
        const char *kind = describe ? "tools" : action;
        const char *cursor = ext_string(args, "cursor");
        if (describe && !cursor && s->tools_cursor)
            cursor = s->tools_cursor;
        rc = list_page(x, s, kind, cursor, yyjson_is_true(yyjson_obj_get(args, "refresh")), call,
                       &page, error);
        if (rc == AH_OK && describe) {
            yyjson_val *tool = find_tool(page, name);
            if (!tool || !ext_tool_allowed(s, name))
                rc = ah_fail(error, AH_NOT_FOUND,
                             "tool not available in this server page; list tools first");
            else {
                result->json = ah_json_write(tool);
                if (!result->json)
                    rc = ah_fail(error, AH_IO, "cannot encode tool schema");
            }
        } else if (rc == AH_OK)
            rc = metadata(s, kind, page, args, result, error);
    }
    if (rc == AH_OK && result->json) {
        yyjson_doc *response = yyjson_read(result->json, strlen(result->json), 0);
        result->is_error = yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(response), "isError"));
        yyjson_doc_free(response);
    }
    if (acquired)
        pthread_mutex_unlock(&s->request_mutex);
    yyjson_doc_free(doc);
    return rc;
}
ah_code ext_mcp_call(void *context, const ah_tool_call *call, ah_tool_result *result,
                     ah_error *error) {
    ah_extensions *x = context;
    yyjson_doc *doc = yyjson_read(call->arguments_json, strlen(call->arguments_json), 0);
    yyjson_val *args = yyjson_doc_get_root(doc);
    const char *name = ext_string(args, "name");
    ext_server *s = NULL;
    bool sent = false;
    ah_code rc = ext_server_get(x, ext_string(args, "server"), &s, error);
    if (rc == AH_OK)
        rc = acquire(s, call, error);
    bool acquired = rc == AH_OK;
    if (rc == AH_OK && !ext_tool_allowed(s, name))
        rc = ah_fail(error, AH_INVALID, "MCP tool disabled by host configuration");
    char *params = rc == AH_OK ? parameters("name", name, yyjson_obj_get(args, "arguments")) : NULL;
    if (rc == AH_OK)
        rc = params ? mcp_client_request(s->client, "tools/call", params, call, &result->json,
                                         &sent, error)
                    : ah_fail(error, AH_IO, "cannot encode MCP tool call");
    free(params);
    if (acquired)
        pthread_mutex_unlock(&s->request_mutex);
    yyjson_doc_free(doc);
    if (rc != AH_OK) {
        result->outcome_unknown = sent;
        return ext_error(result, rc, error);
    }
    return ext_mcp_result(result, call->output_limit, error);
}
