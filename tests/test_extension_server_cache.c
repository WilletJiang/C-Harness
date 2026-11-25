#include "extensions/extensions_internal.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>

/* Link the real extension wrapper to a deterministic transport boundary. */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    unsigned arrived;
    bool released;
} transport_gate;

struct mcp_client {
    char *reply;
    unsigned requests;
    transport_gate *gate;
};

ah_code mcp_client_open(const char *config, size_t limit, bool loopback, mcp_client **out,
                        ah_error *error) {
    (void)config;
    (void)limit;
    (void)loopback;
    (void)out;
    return ah_fail(error, AH_INVALID, "unexpected client creation in cache fixture");
}

ah_code mcp_client_request(mcp_client *client, const char *method, const char *params,
                           const ah_tool_call *call, char **result, bool *sent, ah_error *error) {
    (void)call;
    (void)error;
    assert(!strcmp(method, "tools/list") || !strcmp(method, "resources/list") ||
           !strcmp(method, "resources/templates/list") || !strcmp(method, "tools/call"));
    assert(ah_json_valid(params) && client->reply);
    client->requests++;
    transport_gate *gate = client->gate;
    if (gate) {
        assert(pthread_mutex_lock(&gate->mutex) == 0);
        gate->arrived++;
        assert(pthread_cond_broadcast(&gate->changed) == 0);
        while (!gate->released)
            assert(pthread_cond_wait(&gate->changed, &gate->mutex) == 0);
        assert(pthread_mutex_unlock(&gate->mutex) == 0);
    }
    *result = ah_strdup(client->reply);
    assert(*result);
    *sent = true;
    return AH_OK;
}

void mcp_client_close(mcp_client *client) {
    free(client->reply);
    free(client);
}

static ah_extensions *fixture(size_t budget) {
    ah_extensions *extensions = calloc(1, sizeof(*extensions));
    ext_server *server = calloc(1, sizeof(*server));
    assert(extensions && server);
    assert(pthread_mutex_init(&extensions->mutex, NULL) == 0);
    assert(pthread_mutex_init(&server->request_mutex, NULL) == 0);
    ah_extensions_config_init(&extensions->config);
    extensions->config.max_catalog_bytes = budget;
    server->id = ah_strdup("fixture");
    server->config = yyjson_read("{}", 2, 0);
    server->client = calloc(1, sizeof(*server->client));
    assert(server->id && server->config && server->client);
    extensions->servers = server;
    extensions->server_count = 1;
    return extensions;
}

static char *repeated(size_t size) {
    char *text = malloc(size + 1);
    assert(text);
    memset(text, 'x', size);
    text[size] = 0;
    return text;
}

static void page(mcp_client *client, const char *kind, const char *name, size_t description_bytes,
                 const char *uri) {
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(doc), *items = yyjson_mut_arr(doc),
                   *item = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_val(doc, root, kind, items);
    yyjson_mut_arr_append(items, item);
    yyjson_mut_obj_add_str(doc, item, "name", name);
    char *description = repeated(description_bytes);
    yyjson_mut_obj_add_str(doc, item, "description", description);
    if (uri)
        yyjson_mut_obj_add_str(doc, item, "uri", uri);
    free(client->reply);
    client->reply = ah_mut_write(doc);
    assert(client->reply);
    free(description);
    ah_mut_doc_free(doc);
}

static yyjson_doc *query(ah_extensions *extensions, const char *kind, const char *cursor,
                         bool refresh, ah_code expected) {
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "server", "fixture");
    yyjson_mut_obj_add_str(doc, root, "action", kind);
    yyjson_mut_obj_add_str(doc, root, "cursor", cursor);
    yyjson_mut_obj_add_bool(doc, root, "refresh", refresh);
    char *arguments = ah_mut_write(doc);
    assert(arguments);
    ah_mut_doc_free(doc);
    ah_tool_call call = {
        .arguments_json = arguments, .output_limit = 16384, .model_output_limit = 16384};
    ah_tool_result result = {0};
    ah_error error = {0};
    ah_code code = ext_mcp(extensions, &call, &result, &error);
    if (code != expected)
        fprintf(stderr, "MCP cache expected %s, got %s: %s\n", ah_code_name(expected),
                ah_code_name(code), error.message);
    assert(code == expected);
    yyjson_doc *response = result.json ? yyjson_read(result.json, strlen(result.json), 0) : NULL;
    assert((code == AH_OK) == (response != NULL));
    free(result.json);
    free(arguments);
    return response;
}

static yyjson_val *first(yyjson_doc *doc) {
    yyjson_val *items = yyjson_obj_get(yyjson_doc_get_root(doc), "items");
    assert(yyjson_arr_size(items) == 1);
    return yyjson_arr_get_first(items);
}

static void cursor_budget(void) {
    ah_extensions *extensions = fixture(1024);
    mcp_client *client = extensions->servers->client;
    client->reply = ah_strdup("{\"tools\":[]}");
    char *cursor = repeated(8192);
    query(extensions, "tools", cursor, false, AH_LIMIT);
    assert(client->requests == 1 && extensions->cache_bytes == 0);
    assert(!extensions->servers->tools && !extensions->servers->tools_cursor);
    free(cursor);
    ext_close(extensions);
}

static void cache_reuse_and_replacement(void) {
    ah_extensions *extensions = fixture(1024);
    mcp_client *client = extensions->servers->client;
    page(client, "tools", "original", 300, NULL);
    yyjson_doc *result = query(extensions, "tools", "first", false, AH_OK);
    yyjson_doc_free(result);
    page(client, "tools", "must not refetch", 300, NULL);
    result = query(extensions, "tools", "first", false, AH_OK);
    assert(yyjson_equals_str(yyjson_obj_get(first(result), "name"), "original"));
    assert(client->requests == 1);
    yyjson_doc_free(result);

    page(client, "resources", "other cache", 300, "fixture://resource");
    yyjson_doc_free(query(extensions, "resources", "resource", false, AH_OK));
    size_t retained = extensions->cache_bytes;
    page(client, "tools", "too large", 900, NULL);
    query(extensions, "tools", "second", true, AH_LIMIT);
    assert(extensions->cache_bytes == retained);
    result = query(extensions, "tools", "first", false, AH_OK);
    assert(yyjson_equals_str(yyjson_obj_get(first(result), "name"), "original"));
    assert(client->requests == 3);
    yyjson_doc_free(result);

    page(client, "tools", "replacement", 450, NULL);
    result = query(extensions, "tools", "second", true, AH_OK);
    assert(yyjson_equals_str(yyjson_obj_get(first(result), "name"), "replacement"));
    yyjson_doc_free(result);
    retained = extensions->cache_bytes;
    yyjson_doc_free(query(extensions, "tools", "second", true, AH_OK));
    assert(extensions->cache_bytes == retained && retained <= 1024);
    assert(client->requests == 5);
    ext_close(extensions);
}

static void identities_survive_metadata(void) {
    ah_extensions *extensions = fixture(8192);
    mcp_client *client = extensions->servers->client;
    char *name = repeated(2300), *uri = repeated(3024);
    memcpy(uri, "https://example.test/", strlen("https://example.test/"));
    page(client, "resources", name, 600, uri);
    yyjson_doc *result = query(extensions, "resources", "", false, AH_OK);
    yyjson_val *item = first(result);
    assert(yyjson_equals_str(yyjson_obj_get(item, "name"), name));
    assert(yyjson_equals_str(yyjson_obj_get(item, "uri"), uri));
    assert(yyjson_get_len(yyjson_obj_get(item, "description")) == 512);
    yyjson_doc_free(result);
    free(name);
    free(uri);
    ext_close(extensions);
}

static void resource_templates(void) {
    ah_extensions *extensions = fixture(8192);
    mcp_client *client = extensions->servers->client;
    client->reply = ah_strdup("{\"resourceTemplates\":[{\"name\":\"document\","
                              "\"uriTemplate\":\"fixture://documents/{id}\"}]}");
    yyjson_doc *result = query(extensions, "templates", "", false, AH_OK);
    assert(yyjson_equals_str(yyjson_obj_get(first(result), "uriTemplate"),
                             "fixture://documents/{id}"));
    yyjson_doc_free(result);
    yyjson_doc_free(query(extensions, "templates", "", false, AH_OK));
    assert(client->requests == 1);
    ext_close(extensions);
}

typedef struct {
    ah_extensions *extensions;
    const char *server;
    uint64_t timeout;
    bool failed;
} concurrent_call;

static void *invoke(void *context) {
    concurrent_call *request = context;
    char args[128];
    snprintf(args, sizeof(args), "{\"server\":\"%s\",\"name\":\"slow\",\"arguments\":{}}",
             request->server);
    ah_tool_call call = {.arguments_json = args, .deadline_ms = ah_now_ms() + request->timeout};
    ah_tool_result result = {0};
    ah_error error = {0};
    assert(ext_mcp_call(request->extensions, &call, &result, &error) == AH_OK);
    assert(result.json && !result.outcome_unknown && result.is_error == request->failed);
    free(result.json);
    return NULL;
}

static void wait_for_transports(transport_gate *gate, unsigned count) {
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 3;
    assert(pthread_mutex_lock(&gate->mutex) == 0);
    while (gate->arrived < count)
        assert(pthread_cond_timedwait(&gate->changed, &gate->mutex, &deadline) == 0);
    assert(pthread_mutex_unlock(&gate->mutex) == 0);
}

static void concurrent_requests(bool same_server, bool cancel_waiter) {
    ah_extensions *extensions = fixture(8192), *other = fixture(8192);
    ext_server *second = other->servers;
    other->servers = NULL;
    ext_close(other);
    free(second->id);
    second->id = ah_strdup("second");
    extensions->servers->next = second;
    extensions->server_count++;
    transport_gate gate = {.mutex = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER};
    for (ext_server *server = extensions->servers; server; server = server->next) {
        server->client->gate = &gate;
        server->client->reply = ah_strdup("{\"content\":[]}");
    }
    concurrent_call calls[] = {{extensions, "fixture", 5000, false},
                               {extensions, same_server ? "fixture" : "second",
                                cancel_waiter ? 30 : 5000, cancel_waiter}};
    pthread_t threads[2];
    assert(pthread_create(&threads[0], NULL, invoke, &calls[0]) == 0);
    wait_for_transports(&gate, 1);
    assert(pthread_mutex_trylock(&extensions->servers->request_mutex) == EBUSY);
    assert(pthread_create(&threads[1], NULL, invoke, &calls[1]) == 0);
    if (!same_server)
        wait_for_transports(&gate, 2);
    if (cancel_waiter)
        assert(pthread_join(threads[1], NULL) == 0);
    assert(pthread_mutex_lock(&gate.mutex) == 0);
    gate.released = true;
    assert(pthread_cond_broadcast(&gate.changed) == 0);
    assert(pthread_mutex_unlock(&gate.mutex) == 0);
    assert(pthread_join(threads[0], NULL) == 0);
    if (!cancel_waiter)
        assert(pthread_join(threads[1], NULL) == 0);
    assert(extensions->servers->client->requests == (same_server && !cancel_waiter ? 2U : 1U));
    assert(second->client->requests == (same_server ? 0U : 1U));
    ext_close(extensions);
    assert(pthread_cond_destroy(&gate.changed) == 0);
    assert(pthread_mutex_destroy(&gate.mutex) == 0);
}

int main(void) {
    concurrent_requests(true, false);
    concurrent_requests(false, false);
    concurrent_requests(true, true);
    cursor_budget();
    cache_reuse_and_replacement();
    identities_survive_metadata();
    resource_templates();
    puts("extension server cache tests passed");
    return 0;
}
