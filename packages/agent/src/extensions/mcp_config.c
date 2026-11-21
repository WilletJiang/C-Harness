#include "mcp_internal.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <strings.h>

void mcp_headers_free(struct curl_slist *headers) {
    for (struct curl_slist *p = headers; p; p = p->next)
        mcp_wipe(p->data);
    curl_slist_free_all(headers);
}

ah_code mcp_header_add(struct curl_slist **headers, const char *key, const char *value,
                       ah_error *error) {
    size_t key_len = strlen(key), value_len = strlen(value);
    if (!key_len || key_len > 128 || value_len > 16384)
        return ah_fail(error, AH_INVALID, "Invalid MCP HTTP header");
    for (const unsigned char *p = (const unsigned char *)key; *p; ++p)
        if (!isalnum(*p) && !strchr("!#$%&'*+-.^_`|~", *p))
            return ah_fail(error, AH_INVALID, "Invalid MCP HTTP header name");
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if ((*p < 0x20 && *p != '\t') || *p > 0x7e)
            return ah_fail(error, AH_INVALID, "Invalid MCP HTTP header value");
    char *line = malloc(key_len + value_len + 3);
    if (!line)
        return ah_fail(error, AH_IO, "MCP allocation failed");
    snprintf(line, key_len + value_len + 3, "%s: %s", key, value);
    struct curl_slist *next = curl_slist_append(*headers, line);
    mcp_wipe(line);
    free(line);
    if (!next)
        return ah_fail(error, AH_IO, "MCP allocation failed");
    *headers = next;
    return AH_OK;
}

static bool reserved_header(const char *key) {
    const char *reserved[] = {
        "Host",   "Content-Length",       "Transfer-Encoding", "Connection", "Content-Type",
        "Accept", "MCP-Protocol-Version", "MCP-Session-Id",    "Expect",     "Proxy-Authorization"};
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i)
        if (!strcasecmp(key, reserved[i]))
            return true;
    return false;
}

static bool has_header(struct curl_slist *headers, const char *key) {
    size_t n = strlen(key);
    for (; headers; headers = headers->next)
        if (!strncasecmp(headers->data, key, n) && headers->data[n] == ':')
            return true;
    return false;
}

static ah_code configured_headers(mcp_client *c, yyjson_val *value, bool environment,
                                  ah_error *error) {
    if (!value)
        return AH_OK;
    if (!yyjson_is_obj(value) || yyjson_obj_size(value) > 64)
        return ah_fail(error, AH_INVALID, "Invalid MCP HTTP header configuration");
    size_t i, count;
    yyjson_val *key, *item;
    yyjson_obj_foreach(value, i, count, key, item) {
        const char *name = yyjson_get_str(key), *text = yyjson_get_str(item);
        if (!text || reserved_header(name) || has_header(c->headers, name))
            return ah_fail(error, AH_INVALID, "Invalid or reserved MCP HTTP header");
        if (environment) {
            text = getenv(text);
            if (!text || !*text)
                return ah_fail(error, AH_AUTH,
                               "Configured MCP HTTP credential environment variable is missing");
        }
        ah_code code = mcp_header_add(&c->headers, name, text, error);
        if (code)
            return code;
    }
    return AH_OK;
}

ah_code mcp_http_configure(mcp_client *c, bool allow_loopback, ah_error *error) {
    yyjson_val *root = yyjson_doc_get_root(c->config);
    const char *endpoint = yyjson_get_str(yyjson_obj_get(root, "url"));
    if (!endpoint || !*endpoint || strlen(endpoint) > 8192)
        return ah_fail(error, AH_INVALID, "Invalid MCP endpoint");
    for (const unsigned char *p = (const unsigned char *)endpoint; *p; ++p)
        if (*p <= 0x20 || *p == 0x7f)
            return ah_fail(error, AH_INVALID, "Invalid MCP endpoint");
    CURLU *url = curl_url();
    char *scheme = NULL, *host = NULL, *part = NULL;
    bool valid = url && !curl_url_set(url, CURLUPART_URL, endpoint, 0) &&
                 !curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) &&
                 !curl_url_get(url, CURLUPART_HOST, &host, 0);
    CURLUPart forbidden[] = {CURLUPART_USER, CURLUPART_PASSWORD, CURLUPART_FRAGMENT};
    for (size_t i = 0; valid && i < sizeof(forbidden) / sizeof(forbidden[0]); ++i) {
        if (!curl_url_get(url, forbidden[i], &part, 0))
            valid = false;
        curl_free(part);
        part = NULL;
    }
    if (valid) {
        c->loopback =
            !strcmp(host, "127.0.0.1") || !strcmp(host, "[::1]") || !strcasecmp(host, "localhost");
        bool literal = !strncmp(endpoint, "http://127.0.0.1:", 17) ||
                       !strncmp(endpoint, "http://127.0.0.1/", 17) ||
                       !strncmp(endpoint, "http://[::1]:", 13) ||
                       !strncmp(endpoint, "http://[::1]/", 13) ||
                       !strncmp(endpoint, "http://localhost:", 17) ||
                       !strncmp(endpoint, "http://localhost/", 17);
        valid = !strcasecmp(scheme, "https") ||
                (!strcasecmp(scheme, "http") && allow_loopback && c->loopback && literal);
        if (!strcasecmp(scheme, "http") && !strcasecmp(host, "localhost") &&
            curl_version_info(CURLVERSION_NOW)->version_num < 0x074e00)
            valid = false;
    }
    curl_free(scheme);
    curl_free(host);
    curl_url_cleanup(url);
    if (!valid)
        return ah_fail(error, AH_INVALID,
                       "MCP endpoint requires HTTPS or explicitly allowed literal loopback HTTP");
    ah_code code = configured_headers(c, yyjson_obj_get(root, "http_headers"), false, error);
    if (!code)
        code = configured_headers(c, yyjson_obj_get(root, "env_http_headers"), true, error);
    yyjson_val *token_var = yyjson_obj_get(root, "bearer_token_env_var");
    if (!code && token_var) {
        const char *name = yyjson_get_str(token_var), *token = name ? getenv(name) : NULL;
        if (!token || !*token)
            return ah_fail(error, AH_AUTH,
                           "Configured MCP bearer credential environment variable is missing");
        if (has_header(c->headers, "Authorization"))
            return ah_fail(error, AH_INVALID, "Duplicate MCP authorization configuration");
        size_t n = strlen(token);
        if (n > 16300)
            return ah_fail(error, AH_INVALID, "Invalid MCP bearer credential");
        char *value = malloc(n + 8);
        if (!value)
            return ah_fail(error, AH_IO, "MCP allocation failed");
        snprintf(value, n + 8, "Bearer %s", token);
        code = mcp_header_add(&c->headers, "Authorization", value, error);
        mcp_wipe(value);
        free(value);
    }
    return code;
}

static bool timeout(yyjson_val *root, const char *key, uint64_t *out) {
    yyjson_val *value = yyjson_obj_get(root, key);
    if (!value)
        return true;
    double seconds = yyjson_get_num(value);
    if (!yyjson_is_num(value) || !isfinite(seconds) || seconds <= 0 || seconds > 86400)
        return false;
    *out = (uint64_t)(seconds * 1000);
    return *out > 0;
}

ah_code mcp_client_open(const char *json, size_t limit, bool allow_loopback, mcp_client **out,
                        ah_error *error) {
    if (out)
        *out = NULL;
    ah_error_clear(error);
    if (!out || !json || strlen(json) > 1024 * 1024 || limit < 1024 || limit > 128 * 1024 * 1024)
        return ah_fail(error, AH_INVALID, "Invalid MCP configuration or response bound");
    mcp_client *c = calloc(1, sizeof(*c));
    if (!c)
        return ah_fail(error, AH_IO, "MCP allocation failed");
    c->input = c->output = c->diagnostic = -1;
    c->max_bytes = limit;
    c->startup_ms = 10000;
    c->tool_ms = 60000;
    c->config = yyjson_read(json, strlen(json), 0);
    yyjson_val *root = c->config ? yyjson_doc_get_root(c->config) : NULL;
    ah_code code = AH_OK;
    if (!yyjson_is_obj(root) || ah_json_check(root, NULL))
        code = ah_fail(error, AH_INVALID, "Invalid MCP configuration JSON");
    const char *command = yyjson_get_str(yyjson_obj_get(root, "command"));
    const char *url = yyjson_get_str(yyjson_obj_get(root, "url"));
    c->http = url != NULL;
    if (!code && (!!yyjson_obj_get(root, "command") == !!yyjson_obj_get(root, "url") ||
                  (!command && !url) || (command && !*command)))
        code = ah_fail(error, AH_INVALID, "MCP configuration requires exactly one command or URL");
    const char *version = yyjson_get_str(yyjson_obj_get(root, "protocol_version"));
    if (!version && !yyjson_obj_get(root, "protocol_version"))
        version = "2025-11-25";
    if (!code && !mcp_version_supported(version))
        code = ah_fail(
            error, AH_PROTOCOL,
            "Unsupported MCP protocol version; supported: 2025-11-25, 2025-06-18, 2025-03-26");
    if (!code)
        strcpy(c->version, version);
    if (!code && (!timeout(root, "startup_timeout_sec", &c->startup_ms) ||
                  !timeout(root, "tool_timeout_sec", &c->tool_ms)))
        code = ah_fail(error, AH_INVALID, "Invalid MCP timeout configuration");
    const char *unsupported[] = {
        "bearer_token", "oauth",      "oauth_client_id",      "auth",
        "auth_type",    "codex_auth", "requires_openai_auth", "auth_provider"};
    for (size_t i = 0; !code && i < sizeof(unsupported) / sizeof(unsupported[0]); ++i)
        if (yyjson_obj_get(root, unsupported[i]))
            code = ah_fail(error, AH_AUTH,
                           "MCP hosted/OAuth authentication is unsupported; configure an "
                           "environment credential or HTTP header");
    if (!code && c->http)
        code = mcp_http_configure(c, allow_loopback, error);
    if (code) {
        mcp_client_close(c);
        return code;
    }
    *out = c;
    return AH_OK;
}
