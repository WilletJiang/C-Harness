#ifndef AH_EXTENSIONS_H
#define AH_EXTENSIONS_H
#include "ah.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *codex_home; /* NULL: CODEX_HOME, then $HOME/.codex. */
    const char *workspace;  /* Optional, explicit project skill/config root. */
    bool enable_skills;
    bool enable_mcp;
    bool enable_plugins;
    bool allow_http_loopback;
    bool approve_mcp_calls;
    size_t max_catalog_entries;
    size_t max_catalog_bytes;
    size_t max_skill_bytes;
    size_t max_context_bytes; /* Total pinned skill content per session. */
    size_t max_result_bytes;
} ah_extensions_config;

/* All features default off. Enabling registers small discovery tools, but does
 * not scan disk, start servers, or load skill bodies until their first use.
 * Paths/configuration are copied. Extension state is owned by the engine.
 * Re-enable explicitly after reopening an engine. Loaded skills are scoped to
 * their session; fresh subagents never inherit their parent's loaded content. */
void ah_extensions_config_init(ah_extensions_config *config);
ah_code ah_extensions_enable(ah_engine *engine, const ah_extensions_config *config,
                             ah_error *error);

#ifdef __cplusplus
}
#endif
#endif
