#ifndef AH_JOBS_H
#define AH_JOBS_H
#include "ah.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Opt-in host scheduling of the existing execute callback on a bounded worker.
 * This is independent of any remote provider's asynchronous protocol. Callbacks
 * and their contexts MUST be thread-safe and honor cancelled/deadline_ms. They
 * cannot call owner-only engine APIs; checkpoint and yield are NULL. Borrowed
 * call data and context remain alive until completion. All durable intent,
 * receipt and observer work remains on the engine owner. max_parallel_tools
 * bounds background workers across the engine (0 means 1). Each session preserves tool
 * order. ah_run cancels/drains every worker before returning, even on failure;
 * a callback that ignores cancellation can delay shutdown indefinitely. */
ah_code ah_register_background_tool(ah_engine *, const ah_tool *, ah_error *);

/* Thread-safe cancellation of one active operation, using its durable ID from
 * tool.started. AH_NOT_FOUND means it is no longer active (or not dispatched).
 * The owner records its final receipt; interrupted external effects without a
 * trustworthy receipt remain unknown and require explicit resolution. Engine
 * lifetime must cover this call, as for ah_cancel. */
ah_code ah_cancel_job(ah_engine *, const char *operation_id, ah_error *);

#ifdef __cplusplus
}
#endif
#endif
