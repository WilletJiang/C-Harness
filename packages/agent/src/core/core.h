#ifndef AH_CORE_H
#define AH_CORE_H
#include "../internal.h"
#include "../hash.h"
#include "ah/tools.h"
#include "ah/task.h"
#include "ah/agents.h"
#include "ah/context.h"
#include "ah/work.h"
#include <sqlite3.h>
#include <stdatomic.h>
#include <pthread.h>

typedef struct core_steering {
    char *session, *command, *text;
    enum { STEER_UNCHECKED, STEER_NEW, STEER_RECORDED } command_status;
    unsigned turn;
    struct core_steering *next;
} core_steering;
typedef struct {
    ah_tool tool;
    bool background;
    ah_work_config *work;
    char *name, *description, *schema;
} core_tool;
struct ah_engine {
    ah_config config;
    struct ah_search *search;
    char *flow_runner_path;
    struct core_flow_cell *flow_cells;
    ah_agents_config agents;
    bool agents_enabled;
    struct ah_extensions *extensions;
    ah_context_config context_policy;
    bool context_enabled;
    bool context_history;
    void (*yield)(void *);
    void *yield_context;
    sqlite3 *db;
    bool utf8_store;
    bool has_work; /* Derived fast path; never an execution state. */
    int lock_fd;
    core_tool *tools;
    size_t tool_count;
    unsigned inline_tools;
    _Atomic(struct core_jobs *) jobs;
    atomic_bool cancelled;
    bool running;
    bool observing;
    uint64_t deadline;
    uint64_t (*work_clock_ms)(void); /* Wall time for persisted work deadlines. */
    pthread_mutex_t mailbox_lock;
    bool mailbox_initialized;
    char active_session[AH_ID_CAP];
    unsigned active_turn;
    core_steering *mailbox_head, *mailbox_tail;
    atomic_size_t mailbox_bytes;
};
typedef struct {
    char *id, *model, *instructions, *summary;
    char reasoning[16];
    ah_state state;
    unsigned turn, model_calls, tool_calls;
    bool completion_committed;
    struct core_flow_batch *flow_batch;
} core_session;
typedef struct {
    char *id, *call_id, *name, *arguments, *status, *result, *schema;
    int decision;
    bool is_error;
    ah_effect effect;
    bool approval;
    ah_tool_result_format result_format;
    uint64_t generation;
    bool hidden;
    bool durable;
} core_operation;

void core_mailbox_active(ah_engine *, const char *, unsigned);
void core_mailbox_free(ah_engine *);
bool core_model_has_steer(ah_engine *, const char *);
ah_code core_drain_steering(ah_engine *, core_session *, bool, const char *, ah_error *);
ah_code core_sql(ah_engine *, const char *, ah_error *);
ah_code core_prepare(ah_engine *, const char *, sqlite3_stmt **, ah_error *);
ah_code core_done(ah_engine *, sqlite3_stmt *, ah_error *);
ah_code core_begin(ah_engine *, uint64_t *, ah_error *);
ah_code core_commit(ah_engine *, uint64_t, ah_error *);
void core_rollback(ah_engine *);
ah_code core_event(ah_engine *, const char *, const char *, const char *, const char *, ah_error *);
ah_code core_state(ah_engine *, const char *, ah_state, ah_error *);
ah_code core_load(ah_engine *, const char *, core_session *, ah_error *);
void core_session_free(core_session *);
ah_code core_id(char[AH_ID_CAP], ah_error *);
ah_code core_add_item(ah_engine *, const char *, unsigned, const char *, ah_error *);
ah_code core_content_schema(ah_engine *, ah_error *);
ah_code core_content_fork(ah_engine *, const char *, const char *, ah_error *);
ah_code core_content_expand(ah_engine *, const char *, yyjson_mut_doc *, yyjson_mut_val *, size_t *,
                            ah_error *);
ah_code core_context_schema(ah_engine *, int, ah_error *);
ah_code core_context_restore(ah_engine *, ah_error *);
const char *core_context_tool_schema(void);
ah_code core_context_compact(ah_engine *, core_session *, size_t, bool *, ah_error *);
ah_code core_context_fork(ah_engine *, const char *, const char *, ah_error *);
ah_code core_context_recall(void *, const ah_tool_call *, ah_tool_result *, ah_error *);
ah_code core_context_native(ah_engine *, const char *, sqlite3_int64, ah_error *);
ah_code core_pending_operation(ah_engine *, const char *, ah_state, core_operation *, ah_error *);
ah_code core_next_operation(ah_engine *, const char *, core_operation *, ah_error *);
ah_code core_close_interrupted_tools(ah_engine *, const char *, ah_error *);
ah_code core_get_operation(ah_engine *, const char *, const char *, core_operation *, ah_error *);
void core_operation_free(core_operation *);
ah_code core_normalize_result(ah_engine *, const char *, core_operation *, ah_tool_result *,
                              ah_error *);
ah_code core_save_result(ah_engine *, const char *, const core_operation *, const char *, bool,
                         const char *, const char *, ah_error *);
char *core_retry_feedback(ah_engine *, const char *, const core_operation *, const char *);
ah_code core_receipt(ah_engine *, const char *, const core_operation *, const char *, bool,
                     const char *, ah_error *);
ah_code core_set_operation(ah_engine *, const char *, const char *, const char *, ah_error *);
core_tool *core_find_tool(ah_engine *, const char *);
bool core_cancelled(void *);
ah_code core_flow_register(ah_engine *, ah_error *);
void core_flow_close(ah_engine *);
ah_code core_recover(ah_engine *, ah_error *);
ah_code core_migrate_result_refs(ah_engine *, ah_error *);
const char *core_builtin_schema(const char *);
ah_code core_request_tools(const ah_engine *, yyjson_mut_doc *, yyjson_mut_val *, bool, ah_error *);
ah_code core_request(ah_engine *, const core_session *, char **, ah_error *);
ah_code core_accept_response(ah_engine *, core_session *, const char *, ah_error *);
ah_code core_execute_operation(ah_engine *, core_session *, core_operation *, ah_error *);
ah_code core_complete_operation(ah_engine *, core_session *, core_operation *, ah_tool_result *,
                                ah_code, const ah_error *, ah_code, ah_error *);
ah_code core_jobs_init(ah_engine *, ah_error *);
unsigned core_jobs_capacity(ah_engine *);
bool core_jobs_available(ah_engine *);
bool core_jobs_running(ah_engine *, const char *);
ah_code core_jobs_start(ah_engine *, core_session *, core_operation *, core_tool *,
                        const ah_tool_call *, ah_error *);
ah_code core_jobs_advance(ah_engine *, core_session *, bool, ah_error *);
ah_code core_flow_stage(ah_engine *, core_session *, const char *, ah_error *);
ah_code core_flow_dispatch_staged(ah_engine *, core_session *, ah_error *);
void core_flow_clear_staged(core_session *);
ah_code core_jobs_finish(ah_engine *, core_session *, ah_error *);
void core_jobs_wait(ah_engine *, unsigned);
void core_jobs_cancel(ah_engine *, const char *);
ah_code core_jobs_cancel_owned(ah_engine *, const char *, ah_error *);
void core_jobs_close(ah_engine *);
ah_code core_tools_step(ah_engine *, core_session *, bool *, ah_error *);
ah_code core_write_batch(ah_engine *, core_session *, const core_operation *, bool *, ah_error *);
ah_code core_pause_approval(ah_engine *, core_session *, core_operation *, ah_error *);
ah_code core_job_checkpoint(ah_engine *, const char *, const char *, const char *, ah_error *);
bool core_independent_read(ah_engine *, const core_operation *);
ah_code core_collect_reads(ah_engine *, const char *, const char *, const char *, core_operation *,
                           unsigned, unsigned *, ah_error *);
ah_code core_dispatch_reads(ah_engine *, core_session *, core_operation *, unsigned, ah_error *);
ah_code core_record_intents(ah_engine *, core_session *, core_operation *, unsigned, ah_error *);
ah_code core_stage_flow_reads(ah_engine *, core_session *, const core_operation *, ah_error *);
ah_code core_tool_intents(ah_engine *, core_session *, core_operation *, unsigned, ah_error *);
ah_code core_dispatch_tool(ah_engine *, core_session *, core_operation *, bool, bool, ah_error *);
ah_code core_publish_ready(ah_engine *, core_session *, ah_error *);
ah_code core_capture_operation(ah_engine *, core_session *, core_operation *, ah_tool_result *,
                               ah_code, const ah_error *, ah_code, ah_error *);
ah_code core_guard(ah_engine *, ah_error *);
ah_code core_workspace_schema(ah_engine *, ah_error *);
ah_code core_workspace_authorize(ah_engine *, const char *, const core_operation *,
                                 const core_tool *, bool *, ah_error *);
ah_code core_workspace_delegate(ah_engine *, const char *, const char *, yyjson_val *, ah_error *);
char *core_content_result_view(ah_engine *, const core_operation *, const char *, bool, ah_error *);
char *core_result_view(ah_engine *, const char *, const char *, bool);
char *core_observation_view(ah_engine *, const char *, const core_operation *, const char *, bool,
                            char[65], ah_error *);
ah_code core_observation_save(ah_engine *, const char *, const char *, const char *, ah_error *);
ah_code core_read_result(void *, const ah_tool_call *, ah_tool_result *, ah_error *);
char *core_object_string(const char *, const char *);
void core_metric(ah_engine *, const char *, const char *, const char *, uint64_t, uint64_t, size_t,
                 unsigned, ah_code);
uint64_t core_wall_ms(void);
ah_code core_task_allow(ah_engine *, const char *, bool, unsigned, unsigned *, ah_error *);
ah_code core_task_charge(ah_engine *, const char *, bool, unsigned, ah_error *);
ah_code core_task_deadline(ah_engine *, const char *, uint64_t *, ah_error *);
bool core_task_valid_revision(char[65]);
ah_code core_task_recover(ah_engine *, ah_error *);
ah_code core_task_verify(ah_engine *, const char *, const ah_task_verifier *, const char *,
                         ah_error *);
ah_code core_run_transition(ah_engine *, const char *, ah_state, const char *, const char *,
                            ah_error *);
ah_code core_model_intent(ah_engine *, core_session *, ah_error *);
ah_code core_session_step(ah_engine *, core_session *, bool *, ah_error *);
ah_code core_model_prepare(ah_engine *, core_session *, char **, ah_error *);
ah_code core_model_finish(ah_engine *, core_session *, const char *, ah_code, ah_error *);
ah_code core_run_failure(ah_engine *, core_session *, ah_code, ah_error *, bool *);
ah_code core_run(ah_engine *, const char *, bool, ah_error *);
void core_emit_delta(ah_engine *, const char *, const char *, size_t);
#endif
