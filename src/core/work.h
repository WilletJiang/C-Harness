#ifndef AH_CORE_WORK_H
#define AH_CORE_WORK_H

#include "core.h"
#include "ah/work.h"

#define CORE_WORK_JSON_LIMIT 65536
#define CORE_WORK_SIGNAL_LIMIT 16384
#define CORE_WORK_KEYS_LIMIT 128

typedef struct {
    unsigned version, max_restarts, restarts;
    ah_replay replay;
    uint64_t generation, wake_at_ms;
    char *checkpoint_json, *wait_json, *wake_json;
} core_work;

/* State transitions participate in the caller's transaction. */
ah_code core_work_schema(ah_engine *, ah_error *);
ah_code core_work_seed(ah_engine *, const char *, const core_operation *, const core_tool *,
                       ah_error *);
ah_code core_work_load(ah_engine *, const char *, const char *, core_work *, ah_error *);
void core_work_free(core_work *);
ah_code core_work_claim(ah_engine *, const char *, const char *, core_work *, ah_error *);
ah_code core_work_apply(ah_engine *, const char *, const char *, uint64_t, const char *,
                        ah_error *);
ah_code core_work_ready(ah_engine *, const char *, const char *, bool *, ah_error *);
ah_code core_work_recover(ah_engine *, const char *, const char *, bool *, bool *, ah_error *);
ah_code core_work_next_wake(ah_engine *, const char *, uint64_t *, ah_error *);
ah_code core_work_wake(ah_engine *, const char *, const char *, const core_work *, bool *, char **,
                       char **, ah_error *);
ah_code core_work_close_signal(ah_engine *, const char *, const core_work *, const char *,
                               ah_error *);
ah_code core_work_reserve_signal(ah_engine *, const char *, yyjson_val *, ah_error *);
ah_code core_work_calls_schema(ah_engine *, ah_error *);
ah_code core_work_children(ah_engine *, const char *, const char *, uint64_t, yyjson_val *,
                           ah_error *);
ah_code core_work_children_ready(ah_engine *, const char *, const char *, yyjson_val *, bool *,
                                 char **, ah_error *);
ah_code core_work_create_call(ah_engine *, const char *, const char *, const char *, const char *,
                              const char *, char[AH_ID_CAP], ah_error *);
ah_code core_work_host_session(ah_engine *, const char *, bool *, ah_error *);
ah_code core_work_cancel_tree(ah_engine *, const char *, const char *, ah_error *);
ah_code core_work_finish_children(ah_engine *, const char *, const char *, ah_error *);
ah_code core_work_has_children(ah_engine *, const char *, bool *, ah_error *);
ah_code core_work_select(ah_engine *, const char *, core_operation *, bool *, ah_error *);
yyjson_mut_val *core_work_value(yyjson_mut_doc *, const core_operation *);

ah_code core_work_prepare(ah_engine *, core_session *, core_operation *, core_tool *, bool *,
                          ah_error *);
ah_code core_work_frontier(ah_engine *, const char *, core_operation *, unsigned, bool *,
                           ah_error *);
#endif
