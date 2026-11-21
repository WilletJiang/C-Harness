#ifndef AH_CONTEXT_H
#define AH_CONTEXT_H
#include "ah.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t trigger_bytes; /* 0: 3/4 of the engine context limit. */
    size_t target_bytes;  /* 0: 1/2 of the engine context limit. */
    size_t reserve_bytes; /* 0: 1/8 of the engine limit, capped at 64 KiB. */
    unsigned keep_recent_exchanges;
    /* Explicit provider capability. 0 omits context_management. Never inferred
     * from a model name. This is a token threshold, unlike the byte limits. */
    unsigned compact_threshold_tokens;
} ah_context_config;

void ah_context_config_init(ah_context_config *);
/* Opt-in owner configuration, copied by the engine. At settled model boundaries,
 * archive older complete model/tool exchanges; preserve exact user inputs, human
 * replies and loaded skills. Model-visible checkpoints expose a bounded recall
 * tool. No extra model request is made by structural compaction.
 * When explicitly configured, native encrypted compaction items are accepted
 * and replace the represented prefix atomically. */
ah_code ah_context_enable(ah_engine *, const ah_context_config *, ah_error *);
ah_code ah_context_json(ah_engine *, const char *session_id, char **json, ah_error *);

#ifdef __cplusplus
}
#endif
#endif
