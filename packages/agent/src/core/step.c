#include "core.h"
#include "work.h"

/* Both run loops advance the same durable session. The caller only decides
 * whether a model request runs synchronously or occupies an async slot. */
ah_code core_session_step(ah_engine *e, core_session *session, bool *needs_model, ah_error *error) {
    *needs_model = false;
    if (session->state == AH_READY) {
        ah_code code = core_run_transition(e, session->id, AH_RUNNING, "run.started", "{}", error);
        if (code != AH_OK)
            return code;
        session->state = AH_RUNNING;
    }
    if (session->state != AH_RUNNING && session->state != AH_WAITING_AGENTS &&
        session->state != AH_WAITING_WORK) {
        if (core_jobs_running(e, session->id)) {
            core_jobs_cancel(e, session->id);
            return core_jobs_finish(e, session, error);
        }
        return AH_OK;
    }
    if (core_cancelled(e) && !session->flow_batch)
        return ah_fail(error,
                       atomic_load_explicit(&e->cancelled, memory_order_relaxed) ? AH_CANCELLED
                                                                                 : AH_LIMIT,
                       "run cancelled or deadline exceeded");
    bool pending = false;
    ah_code code = core_tools_step(e, session, &pending, error);
    if (code != AH_OK || pending ||
        (session->state != AH_RUNNING && session->state != AH_WAITING_AGENTS &&
         session->state != AH_WAITING_WORK))
        return code;
    if (session->state == AH_WAITING_AGENTS)
        return ah_fail(error, AH_PROTOCOL, "agent wait has no pending operation");
    if (session->state == AH_WAITING_WORK) {
        code = core_run_transition(e, session->id, AH_RUNNING, "work.resumed", "{}", error);
        if (code != AH_OK)
            return code;
        session->state = AH_RUNNING;
    }
    bool host = false;
    code = core_work_host_session(e, session->id, &host, error);
    if (code == AH_OK && !host)
        code = core_drain_inputs(e, session, false, NULL, error);
    if (code == AH_OK)
        *needs_model = true;
    return code;
}

ah_code core_model_prepare(ah_engine *e, core_session *session, char **request, ah_error *error) {
    *request = NULL;
    if (session->model_calls >= e->config.max_model_calls)
        return ah_fail(error, AH_LIMIT, "model call budget exhausted");
    ah_code code = core_task_allow(e, session->id, true, 1, NULL, error);
    if (code == AH_OK)
        code = core_task_deadline(e, session->id, &e->deadline, error);
    if (code == AH_OK)
        code = core_publish_user_items(e, session, error);
    uint64_t assembled = ah_now_us();
    if (code == AH_OK)
        code = core_request(e, session, request, error);
    core_metric(e, session->id, NULL, "request", assembled, ah_now_us(),
                *request ? strlen(*request) : 0, session->model_calls + 1, code);
    if (code == AH_OK)
        code = core_model_intent(e, session, error);
    if (code == AH_OK && core_cancelled(e))
        code = ah_fail(error,
                       atomic_load_explicit(&e->cancelled, memory_order_relaxed) ? AH_CANCELLED
                                                                                 : AH_LIMIT,
                       "cancelled before model dispatch");
    if (code != AH_OK) {
        free(*request);
        *request = NULL;
    }
    return code;
}
