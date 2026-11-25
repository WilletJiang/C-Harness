#include "responses_internal.h"
#include <pthread.h>
#include <stdatomic.h>

struct responses_refresh {
    pthread_t thread;
    CURLM *multi;
    ah_responses_refresh_fn refresh;
    void *context;
    char *rejected, *replacement;
    uint64_t deadline;
    atomic_bool cancelled, done;
    ah_code result;
};

static bool refresh_cancelled(void *context) {
    struct responses_refresh *work = context;
    return atomic_load(&work->cancelled) || ah_now_ms() >= work->deadline;
}

static void *refresh_token(void *context) {
    struct responses_refresh *work = context;
    work->result = work->refresh(work->context, work->rejected, work->deadline,
                                 refresh_cancelled, work, &work->replacement);
    atomic_store(&work->done, true);
#if LIBCURL_VERSION_NUM >= 0x074400
    (void)curl_multi_wakeup(work->multi);
#endif
    return NULL;
}

static void free_refresh(struct responses_refresh *work) {
    responses_wipe(work->rejected);
    responses_wipe(work->replacement);
    free(work->rejected);
    free(work->replacement);
    free(work);
}

static bool start_refresh(ah_responses *p) {
    struct responses_refresh *work = calloc(1, sizeof(*work));
    if (!work)
        return false;
    work->refresh = p->config.refresh;
    work->multi = p->multi;
    work->context = p->config.refresh_context;
    work->deadline = responses_after(ah_now_ms(), 15000);
    work->rejected = ah_strdup(p->token);
    atomic_init(&work->done, false);
    atomic_init(&work->cancelled, false);
    if (!work->rejected || pthread_create(&work->thread, NULL, refresh_token, work)) {
        free_refresh(work);
        return false;
    }
    p->refresh = work;
    return true;
}

bool responses_auth_wait(responses_request *t) {
    ah_responses *p = t->provider;
    if (t->auth_retried || !p->config.refresh)
        return false;
    t->auth_retried = true;
    if (t->auth_generation != p->auth_generation)
        return true;
    if (!p->refresh && !start_refresh(p))
        return false;
    t->waiting_auth = true;
    return true;
}

void responses_auth_poll(ah_responses *p) {
    struct responses_refresh *work = p->refresh;
    if (!work || !atomic_load(&work->done))
        return;
    (void)pthread_join(work->thread, NULL);
    p->refresh = NULL;
    bool cancelled = atomic_load(&work->cancelled);
    bool expired = ah_now_ms() >= work->deadline;
    bool unavailable = expired || work->result == AH_NETWORK || work->result == AH_LIMIT ||
                       work->result == AH_IO || work->result == AH_CANCELLED;
    bool success = !cancelled && work->result == AH_OK && !expired &&
                   responses_valid_token(work->replacement) && strcmp(work->replacement, p->token);
    if (success) {
        responses_replace_token(p, work->replacement);
        work->replacement = NULL;
    }
    free_refresh(work);
    for (responses_request *t = p->requests; t; t = t->next) {
        if (!t->waiting_auth)
            continue;
        if (cancelled && (p->refresh || start_refresh(p)))
            continue;
        t->waiting_auth = false;
        if (!success) {
            t->failure = unavailable ? RESP_REFRESH_UNAVAILABLE : RESP_REFRESH_FAILED;
            ah_fail(&t->error, unavailable ? AH_NETWORK : AH_AUTH, "%s",
                    unavailable ? "provider credential refresh temporarily unavailable" :
                                  "provider credential refresh did not produce a new bearer");
            responses_settle(t);
        }
    }
}

void responses_auth_cancel_unused(ah_responses *p) {
    if (!p->refresh)
        return;
    for (responses_request *t = p->requests; t; t = t->next)
        if (t->waiting_auth)
            return;
    atomic_store(&p->refresh->cancelled, true);
}

void responses_auth_close(ah_responses *p) {
    if (!p->refresh)
        return;
    atomic_store(&p->refresh->cancelled, true);
    (void)pthread_join(p->refresh->thread, NULL);
    free_refresh(p->refresh);
    p->refresh = NULL;
}
