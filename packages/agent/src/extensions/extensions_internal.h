#ifndef AH_EXTENSIONS_INTERNAL_H
#define AH_EXTENSIONS_INTERNAL_H
#include "catalog.h"
#include "mcp.h"
#include "../core/core.h"

typedef struct ext_server {
    pthread_mutex_t request_mutex;
    char *id;
    yyjson_doc *config, *tools, *resources, *prompts, *templates;
    char *tools_cursor, *resources_cursor, *prompts_cursor, *templates_cursor;
    mcp_client *client;
    struct ext_server *next;
} ext_server;

typedef struct ah_extensions {
    pthread_mutex_t mutex;
    ah_engine *engine;
    ah_extensions_config config;
    ext_catalog *catalog;
    ext_server *servers;
    size_t server_count, cache_bytes;
} ah_extensions;

void ext_close(ah_extensions *);
ah_code ext_context_sync(ah_engine *, const char *, ah_error *);
ah_code ext_skill(void *, const ah_tool_call *, ah_tool_result *, ah_error *);
ah_code ext_skill_read(ah_extensions *, const ah_tool_call *, yyjson_val *, ah_tool_result *,
                       ah_error *);
ah_code ext_read_text(const char *, size_t, char **, size_t *, ah_error *);
ah_code ext_read_text_at(const char *, const char *, size_t, char **, size_t *, ah_error *);
ah_code ext_mcp(void *, const ah_tool_call *, ah_tool_result *, ah_error *);
ah_code ext_mcp_call(void *, const ah_tool_call *, ah_tool_result *, ah_error *);
ah_code ext_mcp_result(ah_tool_result *, size_t, ah_error *);
ah_code ext_server_get(ah_extensions *, const char *, ext_server **, ah_error *);
bool ext_tool_allowed(ext_server *, const char *);
bool ext_match(const char *, const char *);
ah_code ext_result(yyjson_mut_doc *, ah_tool_result *, ah_error *);
/* Encode a known local/remote failure as a settled error receipt. */
ah_code ext_error(ah_tool_result *, ah_code, ah_error *);
const char *ext_string(yyjson_val *, const char *);
size_t ext_number(yyjson_val *, const char *, size_t);

#endif
