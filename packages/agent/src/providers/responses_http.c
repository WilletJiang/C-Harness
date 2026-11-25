#include "responses_internal.h"
#include "stream.h"
#include <stdio.h>
#include <strings.h>

ah_code responses_check_stop(responses_request *t) {
    if (t->cancelled && t->cancelled(t->cancel_context) && !responses_ws_steer_pending(t)) {
        t->failure = RESP_CANCELLED;
        return ah_fail(&t->error, AH_CANCELLED, "provider request cancelled");
    }
    uint64_t now = ah_now_ms();
    if (now >= t->deadline) {
        t->failure = RESP_DEADLINE;
        return ah_fail(&t->error, AH_LIMIT, "provider deadline exceeded");
    }
    if (t->active && t->connect_deadline && now >= t->connect_deadline && !t->error.code) {
        curl_off_t pretransfer = 0;
        if (!t->websocket)
            curl_easy_getinfo(t->easy, CURLINFO_PRETRANSFER_TIME_T, &pretransfer);
        if (!pretransfer) {
            t->failure = RESP_CONNECT_TIMEOUT;
            return ah_fail(&t->error, AH_LIMIT, "provider connection timeout");
        }
        t->connect_deadline = 0;
    }
    if (t->active && t->phase_deadline && now >= t->phase_deadline && !t->error.code) {
        t->failure = t->draining ? RESP_DRAIN_TIMEOUT :
                     t->streaming ? RESP_IDLE_TIMEOUT : RESP_FIRST_OUTPUT_TIMEOUT;
        return ah_fail(&t->error, AH_LIMIT, "%s",
                       t->draining ? "provider response did not close after completion" :
                       t->streaming ? "provider stream idle timeout" : "provider first output timeout");
    }
    return t->error.code;
}

static void arm_phase(responses_request *t, uint64_t limit) {
    t->phase_deadline = limit ? responses_after(ah_now_ms(), limit) : 0;
}

static void start_waiting(responses_request *t) {
    if (t->sent)
        return;
    t->sent = true;
    t->connect_deadline = 0;
    arm_phase(t, t->provider->config.first_output_timeout_ms);
    responses_state(t, "waiting", 0);
}

void responses_request_sent(responses_request *t) {
    if (t->measure)
        responses_stamp(&t->measurement.metrics.upload_complete, t->measurement.started);
    start_waiting(t);
}

void responses_stream_activity(responses_request *t) {
    if (t->streaming && !t->draining)
        arm_phase(t, t->provider->config.stream_idle_timeout_ms);
}

static void stream_phase(void *context, ah_stream_phase phase) {
    responses_request *t = context;
    if (t->measure)
        responses_phase(&t->measurement, phase);
    if (phase == AH_STREAM_CONTENT && !t->streaming) {
        t->streaming = true;
        arm_phase(t, t->provider->config.stream_idle_timeout_ms);
        responses_state(t, "streaming", 0);
    } else if (phase == AH_STREAM_TERMINAL && t->stream.terminal) {
        t->draining = true;
        arm_phase(t, t->provider->config.stream_idle_timeout_ms);
        responses_state(t, "draining", 0);
    }
}

static int progress(void *context, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)a;
    (void)b;
    responses_request *t = context;
    if (d > 0 && (uint64_t)d > t->uploaded_bytes)
        t->uploaded_bytes = (size_t)d;
    if (responses_check_stop(t))
        return 1;
    if (c > 0 && d >= c)
        responses_request_sent(t);
    return 0;
}

void responses_stream_restart(responses_request *t) {
    ah_stream_destroy(&t->stream);
    ah_stream_init(&t->stream, t->provider->config.max_event_bytes, t->limit, t->delta, t->delta_context);
    t->stream.phase = stream_phase;
    t->stream.phase_context = t;
    t->streaming = t->draining = false;
    arm_phase(t, t->provider->config.first_output_timeout_ms);
}

size_t responses_http_header(char *bytes, size_t size, size_t count, void *context) {
    responses_request *t = context;
    if (!size || count > SIZE_MAX / size)
        return 0;
    size_t length = size * count;
    if (t->measure && length)
        responses_stamp(&t->measurement.metrics.first_byte, t->measurement.started);
    if (responses_check_stop(t))
        return 0;
    if (length > 65536 - t->header_bytes) {
        ah_fail(&t->error, AH_LIMIT, "HTTP header limit exceeded");
        return 0;
    }
    t->header_bytes += length;
    if (length >= 5 && !memcmp(bytes, "HTTP/", 5)) {
        const char *space = memchr(bytes, ' ', length);
        if (!space || bytes + length - space < 4 || space[1] < '1' || space[1] > '5' ||
            space[2] < '0' || space[2] > '9' || space[3] < '0' || space[3] > '9') {
            ah_fail(&t->error, AH_PROTOCOL, "invalid HTTP status line");
            return 0;
        }
        t->status = (space[1] - '0') * 100 + (space[2] - '0') * 10 + space[3] - '0';
        if (t->status >= 200 && !t->websocket)
            start_waiting(t);
        responses_route_reset(t);
        t->content_type = false;
        t->content_type_seen = false;
        t->header_complete = false;
        t->retry_after_set = false;
    } else if ((length == 2 && bytes[0] == '\r' && bytes[1] == '\n') ||
               (length == 1 && bytes[0] == '\n')) {
        if (!t->header_complete)
            responses_route_commit(t);
        t->header_complete = true;
    } else if (!t->header_complete && length >= sizeof("x-codex-turn-state:") - 1 &&
               !strncasecmp(bytes, "x-codex-turn-state:", sizeof("x-codex-turn-state:") - 1)) {
        size_t name = sizeof("x-codex-turn-state:") - 1;
        responses_route_capture(t, bytes + name, length - name);
    } else if (!t->header_complete && length >= 13 && !strncasecmp(bytes, "Content-Type:", 13)) {
        if (t->content_type_seen) {
            ah_fail(&t->error, AH_PROTOCOL, "duplicate Content-Type header");
            return 0;
        }
        t->content_type_seen = true;
        size_t offset = 13;
        while (offset < length && (bytes[offset] == ' ' || bytes[offset] == '\t'))
            offset++;
        const char *expected = "text/event-stream";
        size_t n = strlen(expected);
        t->content_type =
            length >= offset + n && !strncasecmp(bytes + offset, expected, n) &&
            (length == offset + n || bytes[offset + n] == ';' || bytes[offset + n] == '\r' ||
             bytes[offset + n] == '\n' || bytes[offset + n] == ' ');
    } else if (!t->header_complete && length >= 12 && !strncasecmp(bytes, "Retry-After:", 12)) {
        uint64_t delay;
        if (responses_retry_after(bytes + 12, length - 12, &delay)) {
            uint64_t retry_at = responses_after(ah_now_ms(), delay);
            if (!t->retry_after_set || retry_at > t->retry_after)
                t->retry_after = retry_at;
            t->retry_after_set = true;
        }
    }
    return length;
}

static size_t body(char *bytes, size_t size, size_t count, void *context) {
    responses_request *t = context;
    if (!size || count > SIZE_MAX / size)
        return 0;
    size_t length = size * count;
    if (t->measure) {
        t->measurement.metrics.response_bytes += length;
        if (length)
            responses_stamp(&t->measurement.metrics.first_byte, t->measurement.started);
    }
    if (responses_check_stop(t))
        return 0;
    if (length > t->limit - t->received) {
        ah_fail(&t->error, AH_LIMIT, "response byte limit exceeded");
        return 0;
    }
    t->received += length;
    if (t->status < 200 || t->status >= 300) {
        size_t cap = 16384;
        if (!t->error_body)
            t->error_body = malloc(cap);
        if (t->error_body && length <= cap - t->error_used && t->received <= cap) {
            memcpy(t->error_body + t->error_used, bytes, length);
            t->error_used += length;
        }
        return length;
    }
    if (!t->header_complete || (t->content_type_seen && !t->content_type)) {
        ah_fail(&t->error, AH_PROTOCOL, "expected text/event-stream response");
        return 0;
    }
    if (length)
        responses_stream_activity(t);
    if (ah_stream_feed(&t->stream, bytes, length)) {
        t->error = t->stream.error;
        return 0;
    }
    return length;
}

/* ChatGPT cache affinity uses this header independently of the JSON key. Derive
 * it from each request, not provider state: one transport can serve many sessions. */
static bool request_route(responses_request *t) {
    char *route = t->session_header;
    yyjson_doc *doc = yyjson_read(t->json, t->request_bytes, 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    bool valid = yyjson_is_obj(root) && ah_json_check(root, NULL) == AH_OK;
    if (valid) {
        yyjson_val *previous = yyjson_obj_get(root, "previous_response_id");
        t->use_http = (previous && !yyjson_is_null(previous)) ||
                      yyjson_get_bool(yyjson_obj_get(root, "background")) ||
                      yyjson_obj_get(root, "conversation");
    }
    route[0] = 0;
    yyjson_val *key = valid ? yyjson_obj_get(root, "prompt_cache_key") : NULL;
    if (key && !yyjson_is_null(key)) {
        const char *value = yyjson_get_str(key);
        size_t length = yyjson_get_len(key);
        valid = value && length && length <= 512;
        for (size_t i = 0; valid && i < length; i++)
            valid = (unsigned char)value[i] >= 0x21 && (unsigned char)value[i] <= 0x7e;
        if (valid)
            snprintf(route, 525, "session-id: %s", value);
    }
    yyjson_doc_free(doc);
    return valid;
}

void responses_free_headers(struct curl_slist *headers) {
    for (struct curl_slist *item = headers; item; item = item->next)
        responses_wipe(item->data);
    curl_slist_free_all(headers);
}

char *responses_bearer_header(const char *token) {
    size_t size = strlen(token) + sizeof("Authorization: Bearer ");
    char *header = malloc(size);
    if (header)
        snprintf(header, size, "Authorization: Bearer %s", token);
    return header;
}

static ah_code build_headers(responses_request *t) {
    const char *token = t->provider->token;
    if (t->headers && t->auth_generation == t->provider->auth_generation &&
        (!t->route || t->route_generation == t->route->generation))
        return responses_route_apply(t);
    responses_free_headers(t->headers);
    t->headers = NULL;
    t->route = NULL;
    t->route_sent = false;
    t->auth_generation = t->provider->auth_generation;
#if LIBCURL_VERSION_NUM >= 0x073600
    if (*t->session && *t->session_header &&
        !strcmp(t->session_header + sizeof("session-id: ") - 1, t->session))
        responses_route_bind(t, t->session, t->turn);
#endif
    char *auth = responses_bearer_header(token);
    if (!auth)
        goto failed;
    t->headers = curl_slist_append(NULL, auth);
    responses_wipe(auth);
    free(auth);
    if (!t->headers)
        goto failed;
    const char *extra[] = {"Content-Type: application/json", "Accept: text/event-stream",
                           t->session_header};
    for (size_t i = 0; i < sizeof(extra) / sizeof(extra[0]); i++) {
        if (!*extra[i])
            continue;
        struct curl_slist *next = curl_slist_append(t->headers, extra[i]);
        if (!next)
            goto failed;
        t->headers = next;
    }
    return responses_route_apply(t);
failed:
    return ah_fail(&t->error, AH_IO, "provider header allocation failed");
}

ah_code responses_request_setup(responses_request *t, const char *sid, unsigned turn,
                                ah_error *error) {
    if (!request_route(t))
        return ah_fail(error, AH_INVALID, "invalid provider request");
    if (sid && strlen(sid) == sizeof(t->session) - 1)
        memcpy(t->session, sid, sizeof(t->session));
    t->turn = turn;
    return AH_OK;
}

static ah_code setup_http(responses_request *t) {
    ah_responses *provider = t->provider;
    t->easy = curl_easy_init();
    if (!t->easy)
        goto failed;
#define SET(option, value)                                                                         \
    do {                                                                                           \
        if (curl_easy_setopt(t->easy, option, value) != CURLE_OK)                                  \
            goto failed;                                                                           \
    } while (0)
    SET(CURLOPT_URL, provider->endpoint);
    SET(CURLOPT_ERRORBUFFER, t->curl_detail);
    SET(CURLOPT_POSTFIELDS, t->json);
    SET(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)t->request_bytes);
    SET(CURLOPT_POST, 1L);
    SET(CURLOPT_FOLLOWLOCATION, 0L);
#if LIBCURL_VERSION_NUM >= 0x073600
    SET(CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
#endif
    SET(CURLOPT_SSL_VERIFYPEER, 1L);
    SET(CURLOPT_SSL_VERIFYHOST, 2L);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_NOPROGRESS, 0L);
    SET(CURLOPT_XFERINFOFUNCTION, progress);
    SET(CURLOPT_XFERINFODATA, t);
    SET(CURLOPT_WRITEFUNCTION, body);
    SET(CURLOPT_WRITEDATA, t);
    SET(CURLOPT_HEADERFUNCTION, responses_http_header);
    SET(CURLOPT_HEADERDATA, t);
    SET(CURLOPT_PRIVATE, t);
    if (provider->loopback)
        SET(CURLOPT_PROXY, "");
#if LIBCURL_VERSION_NUM >= 0x075500
    SET(CURLOPT_PROTOCOLS_STR, provider->config.allow_loopback_http ? "https,http" : "https");
#else
    SET(CURLOPT_PROTOCOLS,
        (long)(CURLPROTO_HTTPS | (provider->config.allow_loopback_http ? CURLPROTO_HTTP : 0)));
#endif
#undef SET
    return AH_OK;
failed:
    return ah_fail(&t->error, AH_IO, "provider allocation or setup failed");
}

static ah_code start_http(responses_request *t) {
    ah_responses *provider = t->provider;
    if ((!t->easy && setup_http(t)) || build_headers(t))
        return t->error.code;
    uint64_t now = ah_now_ms();
    uint64_t remaining = now < t->deadline ? t->deadline - now : 1;
    uint64_t connect = provider->config.connect_timeout_ms;
    if (!connect || connect > remaining)
        connect = remaining;
    if (connect < remaining)
        t->connect_deadline = responses_after(now, connect);
    if (curl_easy_setopt(t->easy, CURLOPT_HTTPHEADER, t->headers) != CURLE_OK ||
        curl_easy_setopt(t->easy, CURLOPT_CONNECTTIMEOUT_MS, (long)connect) != CURLE_OK ||
        curl_easy_setopt(t->easy, CURLOPT_TIMEOUT_MS, (long)remaining) != CURLE_OK ||
        curl_multi_add_handle(provider->multi, t->easy) != CURLM_OK)
        return ah_fail(&t->error, AH_IO, "provider attempt setup failed");
    return AH_OK;
}

ah_code responses_attempt_start(responses_request *t) {
    ah_responses *provider = t->provider;
    t->error = (ah_error){0};
    t->failure = RESP_NONE;
    if (t->attempt && responses_check_stop(t))
        return t->error.code;
    responses_route_reset(t);
    t->websocket = responses_ws_selected(t);
    t->use_http |= !t->websocket;
    t->websocket_fallback = t->websocket_rebuild = false;
    t->websocket_submitted = t->websocket_rejected = false;
    t->connect_deadline = 0;
    t->connection = 0;
    ah_code started = t->websocket ? responses_ws_start(t) : start_http(t);
    if (started) {
        t->failure = RESP_LOCAL;
        return started;
    }
    t->active = true;
    t->attempt++;
    t->status = 0;
    t->received = t->header_bytes = t->error_used = 0;
    t->uploaded_bytes = 0;
    t->phase_deadline = 0;
    t->retry_after_set = t->sent = t->streaming = t->draining = false;
    t->content_type = t->content_type_seen = t->header_complete = false;
    t->measurement = (responses_measurement){0};
    t->proxy_connect_status = t->socket_errno = 0;
    t->curl_code = CURLE_OK;
    t->curl_detail[0] = 0;
    t->proxy_used = t->preupload = false;
    ah_stream_init(&t->stream, provider->config.max_event_bytes, t->limit, t->delta,
                   t->delta_context);
    t->stream.phase = stream_phase;
    t->stream.phase_context = t;
    if (t->measure) {
        t->measurement.metrics.attempt = t->attempt;
        t->measurement.metrics.request_bytes = t->request_bytes;
        t->measurement.metrics.transport = t->websocket ? "websocket" : "http";
        t->measurement.started = ah_now_us();
    }
    responses_state(t, "connecting", 0);
    return AH_OK;
}

static void record_connection(responses_request *t, CURL *easy, CURLcode result) {
    t->curl_code = result;
    if (!t->measurement.curl_started && !t->websocket_reused)
        return;
    curl_easy_getinfo(easy, CURLINFO_HTTP_CONNECTCODE, &t->proxy_connect_status);
    t->proxy_used = t->proxy_connect_status != 0;
#if LIBCURL_VERSION_NUM >= 0x080700
    long proxy = 0;
    if (curl_easy_getinfo(easy, CURLINFO_USED_PROXY, &proxy) == CURLE_OK)
        t->proxy_used |= proxy != 0;
#endif
    if (t->websocket)
        return;
    curl_off_t uploaded = -1;
    if (curl_easy_getinfo(easy, CURLINFO_SIZE_UPLOAD_T, &uploaded) == CURLE_OK &&
        uploaded > 0 && (uint64_t)uploaded > t->uploaded_bytes)
        t->uploaded_bytes = (size_t)uploaded;
    if (result == CURLE_OK)
        return;
    curl_easy_getinfo(easy, CURLINFO_OS_ERRNO, &t->socket_errno);
    /* libcurl can finalize phase timestamps even when the handshake failed. */
    t->preupload = uploaded == 0 && !t->uploaded_bytes && !t->sent && !t->received && !t->status;
}

void responses_attempt_finish(responses_request *t, CURLcode curl_result) {
    ah_responses *provider = t->provider;
    responses_check_stop(t);
    if (t->websocket)
        responses_ws_pause_io(t);
    CURL *easy = t->websocket ? responses_ws_easy(t) : t->easy;
    record_connection(t, easy, curl_result);
    if (!t->websocket)
        curl_multi_remove_handle(provider->multi, easy);
    t->active = false;
    if (t->websocket)
        responses_ws_classify(t, curl_result);
    else
        responses_classify(t, curl_result);
    uint64_t now = ah_now_ms();
    bool replay_safe = !t->stream.observed_output && !t->stream.terminal && now < t->deadline;
    bool safe_retry = replay_safe && (!t->websocket_submitted || t->websocket_rejected);
    bool refresh = safe_retry && t->failure == RESP_AUTH && responses_auth_wait(t);
    bool fallback = replay_safe && t->websocket_fallback;
    bool rebuild = replay_safe && t->websocket_rebuild;
    uint64_t retry_at = now;
    bool retry = false;
    bool preupload = responses_preupload_disconnect(t->failure);
    if (safe_retry &&
        !fallback && !rebuild && t->retries < provider->config.max_retries &&
        (!preupload || t->preupload_retries < 2) &&
        responses_transient(t->failure, curl_result)) {
        unsigned backoff = 200U << t->retries;
        retry_at = t->retry_after_set ? t->retry_after :
                   responses_after(now, backoff + ah_now_us() % (backoff / 2 + 1));
        retry = retry_at < t->deadline;
    }
    if (t->measure) {
        t->measurement.metrics.result = t->error.code;
        t->measurement.metrics.http_status = t->status;
        t->measurement.metrics.retry_planned = retry || refresh || fallback || rebuild;
        t->measurement.metrics.terminal_completed = t->stream.terminal;
        responses_observe(provider, easy, &t->measurement);
    }
    if (t->websocket)
        responses_ws_finish(t);
    ah_stream_destroy(&t->stream);
    if (refresh || retry || fallback || rebuild) {
        t->error = (ah_error){0};
        t->use_http |= fallback;
        t->full_websocket_input |= rebuild;
        t->retry_at = retry_at;
        if (retry) {
            ++t->retries;
            t->preupload_retries += preupload;
        }
        responses_state(t, refresh ? "refreshing" : fallback ? "fallback" : "backoff",
                        !refresh && retry_at > now ? retry_at - now : 0);
    } else
        responses_settle(t);
}

void responses_request_free(responses_request *t) {
    if (!t)
        return;
    responses_route_reset(t);
    curl_easy_cleanup(t->easy);
    responses_free_headers(t->headers);
    ah_stream_destroy(&t->stream);
    free(t->response);
    free(t->error_body);
    free(t);
}
