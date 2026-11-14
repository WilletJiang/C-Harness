#include "core.h"
#include "ah/jobs.h"
#include <time.h>

typedef struct core_job {
    ah_engine *engine;
    core_tool *tool;
    char *session, *operation, *arguments;
    ah_tool_call call;
    ah_tool_result result;
    ah_error error;
    ah_code executed;
    pthread_t thread;
    atomic_bool cancelled;
    bool finished;
    ah_code stopped;
    uint64_t began, ended;
    struct core_job *next;
} core_job;

struct core_jobs {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    core_job *head;
    unsigned count;
};

ah_code ah_register_background_tool(ah_engine *e, const ah_tool *tool, ah_error *error) {
    ah_code code = core_guard(e, error);
    if (code != AH_OK)
        return code;
    if (!e->jobs) {
        struct core_jobs *jobs = calloc(1, sizeof(*jobs));
        if (!jobs)
            return ah_fail(error, AH_IO, "cannot allocate tool jobs");
        if (pthread_mutex_init(&jobs->mutex, NULL)) {
            free(jobs);
            return ah_fail(error, AH_IO, "cannot initialize tool job mutex");
        }
        pthread_condattr_t attributes;
        int initialized = pthread_condattr_init(&attributes);
        if (!initialized) {
#ifndef __APPLE__
            initialized = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
#endif
            if (!initialized)
                initialized = pthread_cond_init(&jobs->changed, &attributes);
            pthread_condattr_destroy(&attributes);
        }
        if (initialized) {
            pthread_mutex_destroy(&jobs->mutex);
            free(jobs);
            return ah_fail(error, AH_IO, "cannot initialize tool job completion");
        }
        e->jobs = jobs;
    }
    code = ah_register_tool(e, tool, error);
    if (code == AH_OK)
        e->tools[e->tool_count - 1].background = true;
    return code;
}

static ah_code stop_reason(core_job *job) {
    if (atomic_load_explicit(&job->cancelled, memory_order_relaxed) ||
        atomic_load_explicit(&job->engine->cancelled, memory_order_relaxed))
        return AH_CANCELLED;
    return ah_now_ms() >= job->call.deadline_ms ? AH_LIMIT : AH_OK;
}

static bool cancelled(void *context) {
    return stop_reason(context) != AH_OK;
}

static void *execute(void *context) {
    core_job *job = context;
    job->began = ah_now_us();
    if (cancelled(job)) {
        /* No dispatch occurred: cancellation is a trustworthy failure receipt. */
        job->result.json = core_object_string("error", "cancelled before execution");
        job->result.is_error = true;
        job->executed = AH_OK;
    } else
        job->executed =
            job->tool->tool.execute(job->tool->tool.context, &job->call, &job->result, &job->error);
    job->stopped = stop_reason(job);
    job->ended = ah_now_us();
    pthread_mutex_lock(&job->engine->jobs->mutex);
    job->finished = true;
    pthread_cond_broadcast(&job->engine->jobs->changed);
    pthread_mutex_unlock(&job->engine->jobs->mutex);
    return NULL;
}

bool core_jobs_available(ah_engine *e) {
    unsigned limit = e->config.max_parallel_tools ? e->config.max_parallel_tools : 1;
    return e->jobs && e->jobs->count < limit;
}

bool core_jobs_running(ah_engine *e, const char *sid) {
    if (e->jobs)
        for (core_job *job = e->jobs->head; job; job = job->next)
            if (!sid || !strcmp(job->session, sid))
                return true;
    return false;
}

static void free_job(core_job *job) {
    free(job->session);
    free(job->operation);
    free(job->arguments);
    free(job->result.json);
    free(job);
}

ah_code core_jobs_start(ah_engine *e, core_session *session, core_operation *operation,
                        core_tool *tool, const ah_tool_call *call, ah_error *error) {
    core_job *job = calloc(1, sizeof(*job));
    if (job) {
        job->session = ah_strdup(session->id);
        job->operation = ah_strdup(operation->id);
        job->arguments = ah_strdup(operation->arguments);
    }
    if (job && job->session && job->operation && job->arguments) {
        job->engine = e;
        job->tool = tool;
        job->call = *call;
        job->call.session_id = job->session;
        job->call.operation_id = job->operation;
        job->call.arguments_json = job->arguments;
        job->call.cancelled = cancelled;
        job->call.cancel_context = job;
        job->call.checkpoint = NULL;
        job->call.checkpoint_context = NULL;
        job->call.yield = NULL;
        job->call.yield_context = NULL;
        atomic_init(&job->cancelled, false);
        pthread_mutex_lock(&e->jobs->mutex);
        int launched = pthread_create(&job->thread, NULL, execute, job);
        if (!launched) {
            job->next = e->jobs->head;
            e->jobs->head = job;
            e->jobs->count++;
        }
        pthread_mutex_unlock(&e->jobs->mutex);
        if (!launched)
            return AH_OK;
    }
    if (job)
        free_job(job);
    ah_tool_result result = {.json = core_object_string("error", "cannot start tool worker"),
                             .is_error = true};
    ah_error execution = {0};
    ah_code code =
        core_complete_operation(e, session, operation, &result, AH_OK, &execution, AH_OK, error);
    free(result.json);
    return code;
}

ah_code ah_cancel_job(ah_engine *e, const char *operation, ah_error *error) {
    if (!e || !operation)
        return ah_fail(error, AH_INVALID, "engine and operation required");
    if (!e->jobs)
        return ah_fail(error, AH_NOT_FOUND, "tool job is not active");
    pthread_mutex_lock(&e->jobs->mutex);
    core_job *job = e->jobs->head;
    while (job && strcmp(job->operation, operation))
        job = job->next;
    if (job) {
        atomic_store_explicit(&job->cancelled, true, memory_order_relaxed);
        pthread_cond_broadcast(&e->jobs->changed);
    }
    pthread_mutex_unlock(&e->jobs->mutex);
    if (!job)
        return ah_fail(error, AH_NOT_FOUND, "tool job is not active");
    ah_error_clear(error);
    return AH_OK;
}

void core_jobs_cancel(ah_engine *e, const char *sid) {
    if (!e->jobs)
        return;
    pthread_mutex_lock(&e->jobs->mutex);
    for (core_job *job = e->jobs->head; job; job = job->next)
        if (!sid || !strcmp(job->session, sid))
            atomic_store_explicit(&job->cancelled, true, memory_order_relaxed);
    pthread_cond_broadcast(&e->jobs->changed);
    pthread_mutex_unlock(&e->jobs->mutex);
}

void core_jobs_wait(ah_engine *e, unsigned milliseconds) {
    if (!e->jobs || !milliseconds)
        return;
    struct core_jobs *jobs = e->jobs;
    pthread_mutex_lock(&jobs->mutex);
    bool finished = false;
    for (core_job *job = jobs->head; job; job = job->next)
        finished |= job->finished;
    if (jobs->head && !finished) {
#ifdef __APPLE__
        struct timespec delay = {.tv_sec = milliseconds / 1000,
                                 .tv_nsec = (long)(milliseconds % 1000) * 1000000};
        pthread_cond_timedwait_relative_np(&jobs->changed, &jobs->mutex, &delay);
#else
        struct timespec deadline;
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += milliseconds / 1000;
        deadline.tv_nsec += (long)(milliseconds % 1000) * 1000000;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&jobs->changed, &jobs->mutex, &deadline);
#endif
    }
    pthread_mutex_unlock(&jobs->mutex);
}

ah_code core_jobs_finish(ah_engine *e, core_session *session, ah_error *error) {
    if (!e->jobs)
        return AH_OK;
    struct core_jobs *jobs = e->jobs;
    pthread_mutex_lock(&jobs->mutex);
    core_job **slot = &jobs->head;
    while (*slot && strcmp((*slot)->session, session->id))
        slot = &(*slot)->next;
    core_job *job = *slot;
    if (!job || !job->finished) {
        pthread_mutex_unlock(&jobs->mutex);
        return AH_OK;
    }
    *slot = job->next;
    jobs->count--;
    pthread_mutex_unlock(&jobs->mutex);
    pthread_join(job->thread, NULL);
    core_metric(e, session->id, job->operation, "tool", job->began, job->ended,
                job->result.json ? strlen(job->result.json) : 0, 1, job->executed);
    core_operation operation;
    ah_code code = core_get_operation(e, session->id, job->operation, &operation, error);
    if (code == AH_OK && !strcmp(operation.status, "executing"))
        code = core_complete_operation(e, session, &operation, &job->result, job->executed,
                                       &job->error, job->stopped, error);
    else if (code == AH_OK) {
        /* An explicit agent interruption may already have published a READ
         * failure or marked an effect unknown. Never publish a second output or
         * turn an interrupted effect into permission to replay. */
        uint64_t after = 0;
        code = core_begin(e, &after, error);
        bool valid = job->result.json && strlen(job->result.json) <= e->config.max_output_bytes &&
                     ah_json_valid(job->result.json);
        if (code == AH_OK)
            code = core_event(e, session->id, operation.id, "tool.late_result",
                              valid ? job->result.json : "{\"discarded\":true}", error);
        if (code == AH_OK)
            code = core_commit(e, after, error);
        else
            core_rollback(e);
    }
    core_operation_free(&operation);
    free_job(job);
    return code;
}

void core_jobs_close(ah_engine *e) {
    if (!e->jobs)
        return;
    core_jobs_cancel(e, NULL);
    while (e->jobs->head) {
        core_job *job = e->jobs->head;
        pthread_join(job->thread, NULL);
        e->jobs->head = job->next;
        free_job(job);
    }
    pthread_cond_destroy(&e->jobs->changed);
    pthread_mutex_destroy(&e->jobs->mutex);
    free(e->jobs);
    e->jobs = NULL;
}
