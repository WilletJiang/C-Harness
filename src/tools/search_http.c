#include "search.h"
#include "../providers/http.h"
#include <limits.h>
#include <time.h>

#define SEARCH_BODY_LIMIT (512u * 1024u)

static size_t receive(char *data, size_t size, size_t count, void *context) {
    search_query *q = context;
    size_t n = size * count;
    if (n > SEARCH_BODY_LIMIT - q->used) {
        q->oversized = true;
        return 0;
    }
    if (q->used + n + 1 > q->capacity) {
        size_t capacity = 2 * q->capacity + n + 1;
        if (capacity > SEARCH_BODY_LIMIT + 1)
            capacity = SEARCH_BODY_LIMIT + 1;
        char *p = realloc(q->body, capacity);
        if (!p)
            return 0;
        q->body = p;
        q->capacity = capacity;
    }
    memcpy(q->body + q->used, data, n);
    q->used += n;
    q->body[q->used] = 0;
    return n;
}

static ah_code start(CURLM *multi, const ah_search *s, struct curl_slist *headers, search_query *q,
                     unsigned count, uint64_t remaining, ah_error *error) {
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *body = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, body);
    yyjson_mut_obj_add_str(doc, body, "query", q->query);
    yyjson_mut_obj_add_uint(doc, body, "max_results", count);
    yyjson_mut_obj_add_uint(doc, body, "snippet_max_length", 512);
    char *json = ah_mut_write(doc);
    ah_mut_doc_free(doc);
    CURL *easy = curl_easy_init();
    bool ok = false;
    if (!json || !easy)
        goto done;
#define SET(opt, value)                                                                            \
    if (curl_easy_setopt(easy, opt, value) != CURLE_OK)                                            \
    goto done
    SET(CURLOPT_URL, s->endpoint);
    SET(CURLOPT_HTTPHEADER, headers);
    SET(CURLOPT_COPYPOSTFIELDS, json);
    SET(CURLOPT_USERAGENT, "C-Harness/" AH_VERSION);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_PIPEWAIT, 1L);
    SET(CURLOPT_FOLLOWLOCATION, 0L);
    SET(CURLOPT_ACCEPT_ENCODING, "");
    if (s->loopback) {
        SET(CURLOPT_NOPROXY, "*");
    }
    SET(CURLOPT_TIMEOUT_MS, remaining > LONG_MAX ? LONG_MAX : (long)remaining);
    SET(CURLOPT_WRITEFUNCTION, receive);
    SET(CURLOPT_WRITEDATA, q);
    SET(CURLOPT_HEADERFUNCTION, search_header);
    SET(CURLOPT_HEADERDATA, &q->headers);
    SET(CURLOPT_PRIVATE, q);
#undef SET
    ok = curl_multi_add_handle(multi, easy) == CURLM_OK;
done:
    free(json);
    if (!ok) {
        curl_easy_cleanup(easy);
        return ah_fail(error, AH_IO, "cannot start web search request");
    }
    q->easy = easy;
    q->attempts++;
    return AH_OK;
}

static void detach(CURLM *multi, search_query *q) {
    curl_multi_remove_handle(multi, q->easy);
    curl_easy_cleanup(q->easy);
    q->easy = NULL;
    free(q->body);
    q->body = NULL;
    q->used = q->capacity = 0;
    q->oversized = false;
    q->headers = (search_headers){0};
}

static bool temporary(CURLcode code, long status) {
    return status == 429 || status == 500 || status == 502 || status == 503 || status == 504 ||
           status == 529 || code == CURLE_COULDNT_CONNECT || code == CURLE_RECV_ERROR ||
           code == CURLE_SEND_ERROR || code == CURLE_PARTIAL_FILE || code == CURLE_GOT_NOTHING ||
           code == CURLE_COULDNT_RESOLVE_HOST || code == CURLE_HTTP2 || code == CURLE_HTTP2_STREAM;
}

static ah_code finish(CURLM *multi, search_query *q, CURLcode transport, unsigned count,
                      size_t budget, ah_error *error) {
    long status = 0;
    curl_easy_getinfo(q->easy, CURLINFO_RESPONSE_CODE, &status);
    q->http_status = (int)status;
    q->transport = transport;
    if ((status == 429 || status == 529) && !q->headers.retry_ms && q->body) {
        yyjson_doc *doc = yyjson_read(q->body, q->used, 0);
        uint64_t seconds =
            yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(doc), "retry_after_seconds"));
        if (seconds <= UINT_MAX)
            q->headers.retry_ms = seconds * 1000;
        yyjson_doc_free(doc);
    }
    q->retry_ms = q->headers.retry_ms;
    ah_code code = AH_OK;
    if (q->oversized)
        q->error = "response_too_large";
    else if (transport == CURLE_OK && status == 200)
        code = search_parse_hits(q, count, budget, error);
    else if (q->attempts < 3 && temporary(transport, status))
        q->ready_at = ah_now_ms() + (250u << (q->attempts - 1));
    else if (status == 429)
        q->error = "rate_limited";
    else if (status == 402)
        q->error = "payment_required";
    else if (status == 401 || status == 403)
        q->error = "access_denied";
    else if (transport == CURLE_OPERATION_TIMEDOUT)
        q->error = "deadline";
    else
        q->error = transport == CURLE_OK ? "upstream_http_error" : "transport_error";
    search_release(&q->headers, status, q->attempts);
    detach(multi, q);
    return code;
}

static void stop_pending(search_query *qs, unsigned n, const char *reason, uint64_t retry_ms) {
    for (unsigned i = 0; i < n; i++) {
        if (!qs[i].easy && !qs[i].error && !qs[i].hits) {
            qs[i].error = reason;
            qs[i].retry_ms = retry_ms;
        }
    }
}

/* One multi handle per batch multiplexes transfers and owns its connection pool.
 * All batches share admission through the process-wide keyless limits. */
ah_code search_transfer(const ah_search *s, const ah_tool_call *call, search_query *qs, unsigned n,
                        unsigned count, size_t budget, search_stats *stats, ah_error *error) {
    ah_code code = ah_http_init(error);
    if (code != AH_OK)
        return code;
    struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
    struct curl_slist *both =
        headers ? curl_slist_append(headers, "X-Keenable-Title: C Harness") : NULL;
    CURLM *multi = both ? curl_multi_init() : NULL;
    if (!multi) {
        curl_slist_free_all(headers);
        return ah_fail(error, AH_IO, "cannot allocate web search transport");
    }
    const char *interrupted = NULL;
    unsigned active = 0;
    while (code == AH_OK) {
        uint64_t now = ah_now_ms(), deadline = call->deadline_ms ? call->deadline_ms : UINT64_MAX;
        if (now >= deadline || (call->cancelled && call->cancelled(call->cancel_context))) {
            interrupted = now >= deadline ? "deadline" : "cancelled";
            break;
        }
        uint64_t delay = deadline - now < 50 ? deadline - now : 50;
        bool pending = false;
        for (unsigned i = 0; i < n; i++) {
            search_query *q = &qs[i];
            if (q->easy || q->hits || q->error)
                continue;
            pending = true;
            /* Let the first request establish the connection and report the
             * IP's actual hourly balance before queuing more work. */
            if (active && stats->attempts == 1)
                break;
            if (q->ready_at > now) {
                if (q->ready_at - now < delay)
                    delay = q->ready_at - now;
                continue;
            }
            uint64_t wait = search_admit();
            if (wait) {
                if (wait >= deadline - now && wait > 50)
                    stop_pending(qs, n, "rate_limited", wait);
                if (wait < delay)
                    delay = wait;
                break;
            }
            code = start(multi, s, headers, q, count, deadline - now, error);
            if (code != AH_OK) {
                search_release(NULL, 0, 0);
                break;
            }
            active++;
            stats->attempts++;
            if (stats->peak_in_flight < active)
                stats->peak_in_flight = active;
        }
        if (code != AH_OK || (!active && !pending))
            break;
        int running = 0;
        if (curl_multi_perform(multi, &running) != CURLM_OK) {
            code = ah_fail(error, AH_NETWORK, "cannot advance web search transfers");
            break;
        }
        CURLMsg *msg;
        int left;
        bool completed = false;
        while ((msg = curl_multi_info_read(multi, &left))) {
            if (msg->msg != CURLMSG_DONE)
                continue;
            search_query *q = NULL;
            curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &q);
            code = finish(multi, q, msg->data.result, count, budget, error);
            active--;
            completed = true;
            if (q->http_status == 429)
                stats->rate_limits++;
            if (q->http_status == 402)
                stop_pending(qs, n, "payment_required", 0);
            if (q->http_status == 401 || q->http_status == 403)
                stop_pending(qs, n, "access_denied", 0);
            if (code != AH_OK)
                break;
        }
        if (completed || code != AH_OK)
            continue;
        if (active) {
            if (curl_multi_poll(multi, NULL, 0, (int)delay, NULL) != CURLM_OK)
                code = ah_fail(error, AH_NETWORK, "cannot poll web search transfers");
        } else {
            struct timespec pause = {.tv_nsec = (long)delay * 1000000};
            nanosleep(&pause, NULL);
        }
    }
    for (unsigned i = 0; i < n; i++)
        if (qs[i].easy) {
            search_release(NULL, 0, 0);
            detach(multi, &qs[i]);
        }
    if (interrupted)
        stop_pending(qs, n, interrupted, 0);
    curl_multi_cleanup(multi);
    curl_slist_free_all(headers);
    return code;
}
