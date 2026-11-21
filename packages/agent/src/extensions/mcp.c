#include "mcp_internal.h"
#include <stdio.h>

void mcp_wipe(char *text) {
    if (text) {
        volatile unsigned char *p = (volatile unsigned char *)text;
        for (size_t n = strlen(text); n; --n)
            *p++ = 0;
    }
}

void mcp_buffer_free(mcp_buffer *buffer) {
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

ah_code mcp_buffer_add(mcp_buffer *b, const char *data, size_t length, size_t limit,
                       ah_error *error) {
    if (b->length > limit || length > limit - b->length)
        return ah_fail(error, AH_LIMIT, "MCP response byte limit exceeded");
    size_t need = b->length + length + 1;
    if (need > b->capacity) {
        size_t capacity = b->capacity ? b->capacity : 4096;
        while (capacity < need && capacity <= limit / 2)
            capacity *= 2;
        if (capacity < need || capacity > limit + 1)
            capacity = need;
        char *next = realloc(b->data, capacity);
        if (!next)
            return ah_fail(error, AH_IO, "MCP allocation failed");
        b->data = next;
        b->capacity = capacity;
    }
    memcpy(b->data + b->length, data, length);
    b->length += length;
    b->data[b->length] = 0;
    return AH_OK;
}

bool mcp_version_supported(const char *v) {
    return v && (!strcmp(v, "2025-11-25") || !strcmp(v, "2025-06-18") || !strcmp(v, "2025-03-26"));
}

ah_code mcp_stop(mcp_exchange *x) {
    if (x->error.code)
        return x->error.code;
    if (x->call && x->call->cancelled && x->call->cancelled(x->call->cancel_context))
        return ah_fail(&x->error, AH_CANCELLED, "MCP request cancelled");
    if (ah_now_ms() >= x->deadline)
        return ah_fail(&x->error, AH_LIMIT, "MCP request deadline exceeded");
    return AH_OK;
}

void mcp_yield(mcp_exchange *x) {
    if (x->call && x->call->yield)
        x->call->yield(x->call->yield_context);
}

/* Never propagate server error strings: peers can echo credentials in errors. */
ah_code mcp_message(mcp_exchange *x, const char *data, size_t length, char **reply) {
    *reply = NULL;
    yyjson_doc *doc = yyjson_read(data, length, 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *id = yyjson_obj_get(root, "id"), *method = yyjson_obj_get(root, "method");
    ah_code code = AH_OK;
    if (!yyjson_is_obj(root) || ah_json_check(root, NULL) ||
        !yyjson_equals_str(yyjson_obj_get(root, "jsonrpc"), "2.0")) {
        code = ah_fail(&x->error, AH_PROTOCOL, "Invalid MCP JSON-RPC message");
    } else if (method) {
        if (!yyjson_is_str(method) || yyjson_obj_get(root, "result") ||
            yyjson_obj_get(root, "error") || (id && !yyjson_is_str(id) && !yyjson_is_int(id))) {
            code = ah_fail(&x->error, AH_PROTOCOL, "Invalid MCP server request");
        } else if (id) {
            yyjson_mut_doc *out = ah_mut_doc_new();
            if (out) {
                yyjson_mut_val *object = yyjson_mut_obj(out);
                yyjson_mut_doc_set_root(out, object);
                yyjson_mut_obj_add_str(out, object, "jsonrpc", "2.0");
                yyjson_mut_obj_add_val(out, object, "id", yyjson_val_mut_copy(out, id));
                if (yyjson_equals_str(method, "ping"))
                    yyjson_mut_obj_add_obj(out, object, "result");
                else {
                    yyjson_mut_val *err = yyjson_mut_obj_add_obj(out, object, "error");
                    yyjson_mut_obj_add_int(out, err, "code", -32601);
                    yyjson_mut_obj_add_str(out, err, "message", "Client method not supported");
                }
                *reply = ah_mut_write(out);
                ah_mut_doc_free(out);
            }
            if (!*reply)
                code = ah_fail(&x->error, AH_IO, "MCP allocation failed");
        }
    } else if (!yyjson_is_uint(id) || yyjson_get_uint(id) != x->id || x->done) {
        code = ah_fail(&x->error, AH_PROTOCOL, "Unexpected MCP response identifier");
    } else {
        yyjson_val *result = yyjson_obj_get(root, "result"), *err = yyjson_obj_get(root, "error");
        if ((result != NULL) == (err != NULL) || (result && !yyjson_is_obj(result)) ||
            (err && (!yyjson_is_obj(err) || !yyjson_is_int(yyjson_obj_get(err, "code")) ||
                     !yyjson_is_str(yyjson_obj_get(err, "message"))))) {
            code = ah_fail(&x->error, AH_PROTOCOL, "Invalid MCP response envelope");
        } else if (err) {
            char value[160];
            snprintf(value, sizeof(value),
                     "{\"isError\":true,\"error\":{\"code\":%lld,\"message\":\"MCP server rejected "
                     "the request\"}}",
                     (long long)yyjson_get_sint(yyjson_obj_get(err, "code")));
            x->result = ah_strdup(value);
            x->done = true;
        } else {
            x->result = ah_json_write(result);
            x->done = true;
        }
        if (x->done && !x->result)
            code = ah_fail(&x->error, AH_IO, "MCP allocation failed");
    }
    yyjson_doc_free(doc);
    return code;
}

static char *request_json(uint64_t id, const char *method, const char *params) {
    yyjson_doc *input = yyjson_read(params, strlen(params), 0);
    yyjson_mut_doc *doc = ah_mut_doc_new();
    char *wire = NULL;
    if (doc && input) {
        yyjson_mut_val *root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
        yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
        yyjson_mut_obj_add_uint(doc, root, "id", id);
        yyjson_mut_obj_add_str(doc, root, "method", method);
        yyjson_mut_obj_add_val(doc, root, "params",
                               yyjson_val_mut_copy(doc, yyjson_doc_get_root(input)));
        wire = ah_mut_write(doc);
    }
    yyjson_doc_free(input);
    ah_mut_doc_free(doc);
    return wire;
}

static uint64_t deadline(const ah_tool_call *call, uint64_t timeout) {
    uint64_t now = ah_now_ms(), until = now > UINT64_MAX - timeout ? UINT64_MAX : now + timeout;
    return call && call->deadline_ms && call->deadline_ms < until ? call->deadline_ms : until;
}

static ah_code exchange(mcp_client *c, const char *method, const char *params,
                        const ah_tool_call *call, uint64_t until, char **result, bool *sent,
                        ah_error *error) {
    mcp_exchange x = {.client = c, .call = call, .deadline = until, .id = ++c->next_id};
    char *wire = request_json(x.id, method, params);
    ah_code code = wire ? mcp_stop(&x) : ah_fail(&x.error, AH_IO, "MCP allocation failed");
    if (!code)
        code = c->http ? mcp_http_exchange(&x, wire) : mcp_stdio_exchange(&x, wire);
    free(wire);
    if (sent)
        *sent = x.sent;
    if (code) {
        free(x.result);
        if (error)
            *error = x.error;
    } else
        *result = x.result;
    return code;
}

static ah_code initialize(mcp_client *c, const ah_tool_call *call, ah_error *error) {
    uint64_t until = deadline(call, c->startup_ms);
    ah_code code = c->http ? AH_OK : mcp_stdio_start(c, error);
    if (code)
        return code;
    char params[200], *result = NULL;
    snprintf(params, sizeof(params),
             "{\"protocolVersion\":\"%s\",\"capabilities\":{},"
             "\"clientInfo\":{\"name\":\"ah\",\"version\":\"" AH_VERSION "\"}}",
             c->version);
    code = exchange(c, "initialize", params, call, until, &result, NULL, error);
    if (code)
        return code;
    yyjson_doc *doc = yyjson_read(result, strlen(result), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *version = yyjson_get_str(yyjson_obj_get(root, "protocolVersion"));
    yyjson_val *caps = yyjson_obj_get(root, "capabilities"),
               *info = yyjson_obj_get(root, "serverInfo");
    if (!mcp_version_supported(version))
        code = ah_fail(error, AH_PROTOCOL, "Unsupported MCP negotiated protocol version");
    else if (!yyjson_is_obj(caps) || !yyjson_is_obj(info) ||
             !yyjson_is_str(yyjson_obj_get(info, "name")) ||
             !yyjson_is_str(yyjson_obj_get(info, "version")))
        code = ah_fail(error, AH_PROTOCOL, "Invalid MCP initialization capabilities");
    else {
        strcpy(c->version, version);
        c->tools = yyjson_is_obj(yyjson_obj_get(caps, "tools"));
        c->resources = yyjson_is_obj(yyjson_obj_get(caps, "resources"));
        c->prompts = yyjson_is_obj(yyjson_obj_get(caps, "prompts"));
    }
    yyjson_doc_free(doc);
    free(result);
    if (code)
        return code;
    mcp_exchange x = {.client = c, .call = call, .deadline = until};
    const char *wire = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    code = c->http ? mcp_http_exchange(&x, wire) : mcp_stdio_send(&x, wire);
    free(x.result);
    if (code && error)
        *error = x.error;
    c->initialized = code == AH_OK;
    return code;
}

static void disconnect(mcp_client *c) {
    if (c->http)
        mcp_http_close(c);
    else
        mcp_stdio_close(c);
    c->initialized = false;
}

ah_code mcp_client_request(mcp_client *c, const char *method, const char *params,
                           const ah_tool_call *call, char **result, bool *sent, ah_error *error) {
    if (result)
        *result = NULL;
    if (sent)
        *sent = false;
    ah_error_clear(error);
    if (!c || !method || !*method || !result || !sent)
        return ah_fail(error, AH_INVALID, "Invalid MCP request arguments");
    if (c->busy)
        return ah_fail(error, AH_BUSY, "MCP client already has an active request");
    if (!strcmp(method, "initialize") || !strncmp(method, "notifications/", 14))
        return ah_fail(error, AH_INVALID, "MCP lifecycle methods are managed by the transport");
    if (!params)
        params = "{}";
    if (strlen(params) > c->max_bytes || strlen(method) > 256)
        return ah_fail(error, AH_LIMIT, "MCP request byte limit exceeded");
    yyjson_doc *doc = yyjson_read(params, strlen(params), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    bool valid = yyjson_is_obj(root) && !ah_json_check(root, NULL);
    yyjson_doc_free(doc);
    if (!valid)
        return ah_fail(error, AH_INVALID, "MCP params must be a JSON object");
    mcp_exchange guard = {.client = c, .call = call, .deadline = deadline(call, c->tool_ms)};
    ah_code code = mcp_stop(&guard);
    if (code) {
        if (error)
            *error = guard.error;
        return code;
    }
    c->busy = true;
    if (!c->initialized) {
        ah_tool_call startup_call = call ? *call : (ah_tool_call){0};
        startup_call.deadline_ms = guard.deadline;
        code = initialize(c, &startup_call, error);
    }
    if (!code && ((!strncmp(method, "tools/", 6) && !c->tools) ||
                  (!strncmp(method, "resources/", 10) && !c->resources) ||
                  (!strncmp(method, "prompts/", 8) && !c->prompts)))
        code = ah_fail(error, AH_PROTOCOL, "MCP server did not advertise the requested capability");
    if (!code)
        code = exchange(c, method, params, call, guard.deadline, result, sent, error);
    if (code)
        disconnect(c); /* An abandoned response can never satisfy a future request. */
    c->busy = false;
    return code;
}

void mcp_client_close(mcp_client *c) {
    if (!c)
        return;
    disconnect(c);
    mcp_headers_free(c->headers);
    if (c->config) {
        yyjson_val *root = yyjson_doc_get_root(c->config);
        /* Parsed string storage belongs to this private configuration document. */
        yyjson_val *end = root + yyjson_doc_get_val_count(c->config);
        for (yyjson_val *v = root; v < end; ++v)
            if (yyjson_is_str(v))
                mcp_wipe((char *)yyjson_get_str(v));
        yyjson_doc_free(c->config);
    }
    free(c);
}
