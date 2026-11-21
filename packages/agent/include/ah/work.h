#ifndef AH_WORK_H
#define AH_WORK_H
#include "ah.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { AH_REPLAY_NEVER = 0, AH_REPLAY_SAFE = 1 } ah_replay;
/* A migration transforms stored state only. It must not perform effects. The
 * returned checkpoint is malloc-owned JSON object; NULL is invalid on success. */
typedef ah_code (*ah_work_migrate_fn)(void *context, unsigned from_version,
                                      const char *arguments_json, const char *checkpoint_json,
                                      char **new_checkpoint_json, ah_error *error);
struct ah_work_config {
    unsigned version;
    unsigned max_restarts; /* 0 selects 5 interrupted attempts per checkpoint. */
    ah_replay replay;
    ah_work_migrate_fn migrate;
};

/* execute() returns either the ordinary terminal result, or continuation_json:
 * {"checkpoint":{...},"calls":[{"key":"a","tool":"name","arguments":{}}],
 *  "wait":{"signal":"key","until_ms":123,"calls":["a"],"policy":"all"}}
 * Only checkpoint is required; without wait the next step is immediately ready.
 * Conditions are alternatives; child policies are all/any/fail_fast. until_ms is
 * Unix wall time. The owner commits checkpoint/calls/wait atomically, then frees
 * the continuation. Parked invocations retain no worker. Initial checkpoint is
 * "{}"; later callbacks receive stored state and the persisted wake reason.
 * Envelopes are bounded at 64 KiB, signal values at 16 KiB and inboxes at 128
 * single-use keys (including reserved waits). See docs/durable.md for semantics. */

typedef enum {
    AH_SIGNAL_DELIVERED,
    AH_SIGNAL_DUPLICATE,
    AH_SIGNAL_CLOSED,
    AH_SIGNAL_NOT_FOUND
} ah_signal_result;
/* Owner-thread durable inbox. The first value for an operation/key wins;
 * retries report DUPLICATE while open and CLOSED after terminal/expired wait. */
ah_code ah_signal(ah_engine *, const char *session_id, const char *operation_id, const char *key,
                  const char *value_json, ah_signal_result *, ah_error *);

/* Host work uses a fresh session with no submitted input or model history.
 * The first start permanently marks that session as host-only. Repeating a key
 * with the same tool and JSON arguments returns the original operation ID. */
ah_code ah_work_start(ah_engine *, const char *session_id, const char *key, const char *tool,
                      const char *arguments_json, char operation_id[AH_ID_CAP], ah_error *);
/* Drives the ordinary tool executor until completion, a durable wait, approval,
 * input, cancellation, deadline, or an uncertain outcome. Never calls a model. */
ah_code ah_work_run(ah_engine *, const char *session_id, ah_error *);
/* Cancel one operation and its descendants while the owner is quiescent.
 * Unknown external effects remain unknown and still require reconciliation. */
ah_code ah_work_cancel(ah_engine *, const char *session_id, const char *operation_id, ah_error *);
/* Earliest wall-clock timer deadline in milliseconds; zero means no timer. */
ah_code ah_work_next_wake(ah_engine *, const char *session_id, uint64_t *, ah_error *);
/* Snapshot of one operation and its direct children. JSON is malloc-owned. */
ah_code ah_work_json(ah_engine *, const char *session_id, const char *operation_id, char **json,
                     ah_error *);

#ifdef __cplusplus
}
#endif
#endif
