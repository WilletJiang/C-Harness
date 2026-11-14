#ifndef AH_EXTENSION_CATALOG_H
#define AH_EXTENSION_CATALOG_H
#include "ah/extensions.h"
#include "../internal.h"

typedef struct ext_catalog ext_catalog;
ah_code ext_catalog_open(const ah_extensions_config *, ext_catalog **, ah_error *);
void ext_catalog_close(ext_catalog *);
/* kind: skill, mcp, plugin, or all. Optional plugin ID expands only that bundle.
 * Output is bounded metadata JSON with items and pagination, never credentials,
 * command arguments, full skill bodies, or remote tool schemas. */
ah_code ext_catalog_query(ext_catalog *, const char *kind, const char *query,
                          const char *plugin, size_t offset, size_t limit,
                          char **json, ah_error *);
ah_code ext_catalog_skill(ext_catalog *, const char *id, char **path, ah_error *);
/* Normalized MCP config JSON, private to transport: command,args,cwd,env,
 * env_vars,url,http_headers,env_http_headers,bearer_token_env_var,timeouts,
 * enabled_tools,disabled_tools. Resolve plugin root variables before returning.
 * Disabled/unsupported/unavailable entries must fail before execution. */
ah_code ext_catalog_server(ext_catalog *, const char *id, char **json, ah_error *);
#endif
