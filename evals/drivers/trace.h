#ifndef AH_EVAL_TRACE_H
#define AH_EVAL_TRACE_H
#include "ah/ah.h"
#include "ah/responses.h"
typedef struct {
    ah_provider inner;
    ah_async_provider async_inner;
    const char *effort;
    bool omit_cache_key, diagnostics, trace;
    char *comparison_id;
    unsigned request_serial;
} eval_model;
ah_provider eval_model_provider(eval_model *model);
ah_async_provider eval_model_async_provider(eval_model *model);
void eval_model_close(eval_model *model);
void eval_transport_observe(void *, const ah_responses_metrics *);
#endif
