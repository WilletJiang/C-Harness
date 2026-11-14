#ifndef AH_EXTENSION_MCP_H
#define AH_EXTENSION_MCP_H
#include "../internal.h"

typedef struct mcp_client mcp_client;
/* Construction is inert; connect and handshake on the first request. */
ah_code mcp_client_open(const char *config_json, size_t max_bytes, bool allow_http_loopback,
                        mcp_client **out, ah_error *error);
void mcp_client_close(mcp_client *client);
/* Owner thread only. call supplies deadline/cancellation and cooperative model
 * I/O yielding. result is malloc-owned MCP result JSON. sent reports whether
 * the requested method (not initialization) may have reached the server;
 * failed effectful requests with sent=true are never automatically retried. */
ah_code mcp_client_request(mcp_client *client, const char *method, const char *params_json,
                           const ah_tool_call *call, char **result, bool *sent,
                           ah_error *error);
#endif
