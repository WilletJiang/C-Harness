#ifndef AH_MCP_INTERNAL_H
#define AH_MCP_INTERNAL_H
#include "mcp.h"
#include <curl/curl.h>
#include <sys/types.h>

typedef struct {
    char *data;
    size_t length, capacity;
} mcp_buffer;

struct mcp_client {
    yyjson_doc *config;
    size_t max_bytes;
    uint64_t startup_ms, tool_ms, next_id;
    char version[16];
    bool http, loopback, initialized, busy;
    bool tools, resources, prompts;
    pid_t pid;
    int input, output, diagnostic;
    mcp_buffer pending;
    CURL *easy;
    CURLM *multi;
    struct curl_slist *headers;
    char *session;
};

typedef struct {
    mcp_client *client;
    const ah_tool_call *call;
    uint64_t deadline, id;
    size_t received;
    bool sent, done;
    char *result;
    ah_error error;
} mcp_exchange;

void mcp_wipe(char *text);
void mcp_buffer_free(mcp_buffer *buffer);
ah_code mcp_buffer_add(mcp_buffer *, const char *, size_t, size_t, ah_error *);
ah_code mcp_stop(mcp_exchange *exchange);
void mcp_yield(mcp_exchange *exchange);
ah_code mcp_message(mcp_exchange *, const char *, size_t, char **reply);
ah_code mcp_stdio_start(mcp_client *, ah_error *);
ah_code mcp_stdio_exchange(mcp_exchange *, const char *wire);
ah_code mcp_stdio_send(mcp_exchange *, const char *wire);
void mcp_stdio_close(mcp_client *);
ah_code mcp_http_configure(mcp_client *, bool allow_loopback, ah_error *);
ah_code mcp_http_exchange(mcp_exchange *, const char *wire);
void mcp_http_close(mcp_client *);
void mcp_headers_free(struct curl_slist *headers);
ah_code mcp_header_add(struct curl_slist **headers, const char *key, const char *value,
                       ah_error *error);
bool mcp_version_supported(const char *version);
#endif
