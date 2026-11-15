#ifndef AH_SDK_H
#define AH_SDK_H
#include "agents.h"
#include "content.h"
#include "context.h"
#include "extensions.h"
#include "jobs.h"
#include "responses.h"
#include "tools.h"
#include "workspace.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ah_sdk ah_sdk;
typedef struct {
    ah_config engine;
    /* Used when engine.provider.complete is NULL. No credential discovery. */
    ah_responses_config responses;
    /* NULL workspace disables local tools. Exec remains explicit opt-in. */
    ah_local_tools_config local;
    const ah_check_config *checks;
    size_t check_count;
    /* max_active=0 disables subagents. For a custom synchronous provider,
     * supply its matching async provider here before enabling subagents. */
    ah_agents_config agents;
    bool automatic_context;
    ah_context_config context;
    ah_extensions_config extensions;
} ah_sdk_config;

/* Initializes every nested configuration. Writes require approval by default;
 * exec, subagents, automatic context, skills, MCP and plugins default off. */
void ah_sdk_config_init(ah_sdk_config *);
/* Owns the engine, built-in Responses provider and optional local tool host.
 * Construction makes no model request. Existing engine/session APIs operate on
 * ah_sdk_engine(); there is no separate scheduler or session representation.
 * Config strings are consumed/copied during this call. Custom provider/observer
 * contexts remain host-owned and must outlive the SDK. */
ah_code ah_sdk_open(const ah_sdk_config *, ah_sdk **out, ah_error *);
ah_engine *ah_sdk_engine(ah_sdk *);           /* Borrowed; do not ah_close(). */
ah_local_tools *ah_sdk_local_tools(ah_sdk *); /* Borrowed, or NULL. */
/* Owner thread, idle only. Copies the token for the built-in provider. The host
 * owns login/refresh; custom providers manage their own credentials. */
ah_code ah_sdk_set_token(ah_sdk *, const char *, ah_error *);
/* Owner thread, after active run threads have joined. Closes the engine before
 * releasing its hosts. NULL is accepted; custom providers are never closed. */
void ah_sdk_close(ah_sdk *);

#ifdef __cplusplus
}
#endif
#endif
