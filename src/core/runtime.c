#include "core.h"
#include "work.h"
#include "../agents/agents_internal.h"

typedef struct {
    ah_engine *engine;
    const char *session;
} delta_context;
static void on_delta(void *context, const char *text, size_t length) {
    delta_context *sink = context;
    core_emit_delta(sink->engine, sink->session, text, length);
}
ah_code ah_run(ah_engine *e, const char *sid, ah_error *err) {
    ah_code code = core_guard(e, err);
    bool host = false;
    if (code == AH_OK)
        code = core_work_host_session(e, sid, &host, err);
    if (code != AH_OK)
        return code;
    if (!host && e->agents_enabled)
        return agents_run(e, sid, err);
    return core_run(e, sid, host, err);
}
ah_code ah_work_run(ah_engine *e, const char *sid, ah_error *err) {
    ah_code code = core_guard(e, err);
    bool host = false;
    if (code == AH_OK)
        code = core_work_host_session(e, sid, &host, err);
    if (code != AH_OK)
        return code;
    return host ? core_run(e, sid, true, err)
                : ah_fail(err, AH_INVALID, "session is not host work");
}
ah_code ah_work_next_wake(ah_engine *e, const char *sid, uint64_t *at, ah_error *err) {
    ah_code code = core_guard(e, err);
    if (code != AH_OK)
        return code;
    if (!sid || !at)
        return ah_fail(err, AH_INVALID, "session and time output required");
    return core_work_next_wake(e, sid, at, err);
}
ah_code core_run(ah_engine *e, const char *sid, bool host, ah_error *err) {
    ah_code rc = core_guard(e, err);
    if (rc != AH_OK)
        return rc;
    core_session session;
    rc = core_load(e, sid, &session, err);
    if (rc != AH_OK)
        return rc;
    if (session.state == AH_COMPLETED || session.state == AH_WAITING_APPROVAL ||
        session.state == AH_WAITING_INPUT) {
        core_session_free(&session);
        return AH_OK;
    }
    if (session.state != AH_READY && session.state != AH_WAITING_WORK) {
        ah_state state = session.state;
        core_session_free(&session);
        return ah_fail(err, state == AH_OUTCOME_UNKNOWN ? AH_UNCERTAIN : AH_INVALID,
                       "session must be ready; submit or resume explicitly");
    }
    e->running = true;
    core_mailbox_active(e, host ? NULL : sid);
    uint64_t now = ah_now_ms();
    e->deadline = now + e->config.run_timeout_ms;
    if (e->deadline < now)
        e->deadline = UINT64_MAX;
    rc = core_task_allow(e, sid, true, 0, NULL, err);
    if (rc == AH_OK)
        rc = core_task_deadline(e, sid, &e->deadline, err);
    while (rc == AH_OK && (session.state == AH_READY || session.state == AH_RUNNING ||
                           session.state == AH_WAITING_WORK)) {
        bool needs_model = false;
        rc = core_session_step(e, &session, &needs_model, err);
        if (rc != AH_OK)
            break;
        if (!needs_model) {
            if (core_jobs_running(e, sid))
                core_jobs_wait(e, 20);
            else if (session.state == AH_WAITING_WORK)
                break;
            continue;
        }
        if (host) {
            rc = core_run_transition(e, sid, AH_COMPLETED, "work.completed", "{}", err);
            session.state = AH_COMPLETED;
            break;
        }
        char *request = NULL;
        rc = core_model_prepare(e, &session, &request, err);
        if (rc != AH_OK)
            break;
        char *response = NULL;
        delta_context sink = {e, sid};
        uint64_t started = ah_now_us();
        rc = e->config.provider.complete(e->config.provider.context, request, on_delta, &sink,
                                         core_cancelled, e, &response, err);
        core_metric(e, sid, NULL, "model", started, ah_now_us(), response ? strlen(response) : 0,
                    session.model_calls, rc);
        free(request);
        rc = core_model_finish(e, &session, response, rc, err);
        free(response);
    }
    if (core_jobs_running(e, sid))
        core_jobs_cancel(e, sid);
    while (core_jobs_running(e, sid)) {
        core_jobs_wait(e, 20);
        ah_error failure = {0};
        ah_code finished = core_jobs_finish(e, &session, &failure);
        if (finished != AH_OK && (rc == AH_OK || finished == AH_UNCERTAIN || finished == AH_IO)) {
            rc = finished;
            if (err)
                *err = failure;
        }
    }
    rc = core_run_failure(e, &session, rc, err, NULL);
    core_mailbox_active(e, NULL);
    e->deadline = 0;
    e->running = false;
    core_session_free(&session);
    return rc;
}
