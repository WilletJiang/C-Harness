#include "responses_internal.h"

#ifdef AH_HAVE_CURL_WS
#include "responses_ws_context.h"
#include "responses_ws_steer.h"
#include <errno.h>
#include <strings.h>
#include <sys/select.h>

enum { WS_IDLE_CONNECTIONS = 8, WS_UPGRADE_COOLDOWN_MS = 60000, WS_UPGRADE_MS = 2000,
       WS_SESSION_MAX_MS = 55 * 60 * 1000, WS_REUSE_PROBE_MS = 1000 };

struct responses_ws {
    struct responses_ws *next;
    CURL *easy;
    CURLM *multi;
    responses_ws_io *io;
    struct curl_slist *headers;
    uint64_t credential, expires_at, connection, probe_deadline;
    char session[AH_ID_CAP], response_id[257];
    responses_ws_context previous;
    responses_ws_input input;
    responses_ws_steer steering;
    char *send, *message;
    size_t send_size, send_offset, message_size, message_cap, max_send;
    bool added, ready, continued, seen_text, resume_tools;
};

bool ah_responses_websocket_available(void) {
    const curl_version_info_data *version = curl_version_info(CURLVERSION_NOW);
    if (!version || version->version_num < 0x081000 || !version->protocols)
        return false;
    for (const char *const *p = version->protocols; *p; ++p)
        if (!strcmp(*p, "wss"))
            return true;
    return false;
}

bool responses_ws_selected(const responses_request *t) {
    const ah_responses *p = t->provider;
    return p->websocket_available && !t->use_http &&
           (p->config.transport == AH_RESPONSES_WEBSOCKET ||
            (p->config.transport == AH_RESPONSES_AUTO &&
             !strcmp(p->endpoint, AH_RESPONSES_OPENAI_ENDPOINT))) &&
           ah_now_ms() >= p->websocket_unavailable_until;
}

bool responses_ws_steer_pending(responses_request *t) {
    responses_turn_steer *steer = t->steer;
    if (!steer)
        return false;
    struct responses_ws *ws = t->ws;
    if (!ws || !t->active || !t->sent || ws->resume_tools || ws->steering.rejected ||
        steer->hard_cancelled(steer->context) ||
        (t->stream.terminal && !steer->in_flight)) {
        steer->in_flight = false;
        return false;
    }
    if (!steer->in_flight && !steer->peek(steer->context, steer->command))
        return false;
    steer->in_flight = true;
    return true;
}

static void free_socket(struct responses_ws *ws) {
    if (!ws)
        return;
    responses_ws_io_close(ws->io);
    if (ws->added)
        curl_multi_remove_handle(ws->multi, ws->easy);
    curl_easy_cleanup(ws->easy);
    if (ws->multi)
        curl_multi_cleanup(ws->multi);
    responses_free_headers(ws->headers);
    free(ws->send);
    free(ws->message);
    responses_ws_steer_clear(&ws->steering);
    free(ws);
}

static bool append_message(struct responses_ws *ws, const char *bytes, size_t length, size_t limit) {
    size_t needed = ws->message_size + length;
    if (needed > ws->message_cap) {
        size_t capacity = ws->message_cap ? 2 * ws->message_cap : 4096;
        if (capacity < needed)
            capacity = needed;
        if (capacity > limit)
            capacity = limit;
        char *grown = realloc(ws->message, capacity);
        if (!grown)
            return false;
        ws->message = grown;
        ws->message_cap = capacity;
    }
    if (length)
        memcpy(ws->message + ws->message_size, bytes, length);
    ws->message_size = needed;
    return true;
}

static bool session_metadata(yyjson_val *type) {
    return yyjson_equals_str(type, "codex.rate_limits") || yyjson_equals_str(type, "rate_limits.updated") ||
           yyjson_equals_str(type, "codex.response.metadata");
}

static bool idle_is_clean(struct responses_ws *ws) {
    const size_t control_limit = 32768;
    for (unsigned i = 0; i < 4; ++i) {
        char bytes[AH_WS_IO_CHUNK];
        size_t length;
        struct curl_ws_frame frame;
        CURLcode code = responses_ws_io_recv(ws->io, bytes, &length, &frame);
        if (code == CURLE_AGAIN)
            return true;
        if (code != CURLE_OK)
            return false;
        if (!(frame.flags & CURLWS_TEXT) ||
            length > control_limit - ws->message_size ||
            (uint64_t)frame.bytesleft > control_limit - ws->message_size - length ||
            !append_message(ws, bytes, length, control_limit))
            return false;
        if (frame.bytesleft || (frame.flags & CURLWS_CONT))
            continue;
        yyjson_doc *doc = ws->message_size ? yyjson_read(ws->message, ws->message_size, 0) : NULL;
        ws->message_size = 0;
        yyjson_val *event = doc ? yyjson_doc_get_root(doc) : NULL;
        bool allowed = session_metadata(yyjson_obj_get(event, "type")) ||
                       (ws->resume_tools && responses_ws_pending_event(&ws->steering, event));
        yyjson_doc_free(doc);
        if (!allowed)
            return false;
    }
    return false;
}

void responses_ws_clear(ah_responses *p) {
    while (p->idle_websockets) {
        struct responses_ws *ws = p->idle_websockets;
        p->idle_websockets = ws->next;
        free_socket(ws);
    }
    p->websocket_unavailable_until = 0;
}

void responses_ws_shutdown(ah_responses *p) { responses_ws_io_shutdown(p); }

static struct responses_ws *take_idle(responses_request *t) {
    if (!*t->session)
        return NULL;
    ah_responses *p = t->provider;
    uint64_t now = ah_now_ms();
    struct responses_ws **link = &p->idle_websockets;
    while (*link) {
        struct responses_ws *ws = *link;
        if (now >= ws->expires_at || ws->credential != p->auth_generation ||
            responses_ws_io_status(ws->io)) {
            *link = ws->next;
            free_socket(ws);
        } else if (!strcmp(ws->session, t->session)) {
            *link = ws->next;
            ws->next = NULL;
            if (idle_is_clean(ws)) {
                responses_ws_io_resume(ws->io, 0);
                return ws;
            }
            free_socket(ws);
        } else
            link = &ws->next;
    }
    return NULL;
}

static struct responses_ws *connect_socket(responses_request *t) {
    ah_responses *p = t->provider;
    struct responses_ws *ws = calloc(1, sizeof(*ws));
    if (!ws)
        return NULL;
    ws->easy = curl_easy_init();
    /* CONNECT_ONLY requires the easy handle to remain on this multi after upgrade. */
    ws->multi = curl_multi_init();
    ws->credential = p->auth_generation;
    ws->connection = ++p->next_websocket;
    memcpy(ws->session, t->session, sizeof(ws->session));
    char *auth = responses_bearer_header(p->token);
    if (auth) {
        ws->headers = curl_slist_append(NULL, auth);
        responses_wipe(auth);
        free(auth);
    }
    if (!ws->easy || !ws->multi || !ws->headers)
        goto failed;
    if (*t->session_header) {
        struct curl_slist *next = curl_slist_append(ws->headers, t->session_header);
        if (!next)
            goto failed;
        ws->headers = next;
    }
    char url[8193];
    const char *path = strstr(p->endpoint, "://");
    if (!path)
        goto failed;
    bool cleartext = path - p->endpoint == 4;
    if (cleartext && (!p->loopback || !p->config.allow_loopback_http))
        goto failed;
    int url_size = snprintf(url, sizeof(url), "%s%s", cleartext ? "ws" : "wss", path);
    if (url_size < 0 || (size_t)url_size >= sizeof(url))
        goto failed;
    uint64_t now = ah_now_ms();
    uint64_t remaining = t->deadline > now ? t->deadline - now : 1;
    uint64_t timeout = remaining;
    uint64_t connect = p->config.connect_timeout_ms;
    if (connect && connect < timeout)
        timeout = connect;
    if (p->config.transport == AH_RESPONSES_AUTO && timeout > WS_UPGRADE_MS)
        timeout = WS_UPGRADE_MS;
    if (timeout < remaining)
        t->connect_deadline = responses_after(now, timeout);
#define SET(option, value) \
    do { if (curl_easy_setopt(ws->easy, option, value) != CURLE_OK) goto failed; } while (0)
    SET(CURLOPT_URL, url);
    SET(CURLOPT_PROTOCOLS_STR, cleartext ? "ws" : "wss");
    SET(CURLOPT_HTTPHEADER, ws->headers);
    SET(CURLOPT_CONNECT_ONLY, 2L);
    SET(CURLOPT_WS_OPTIONS, (long)CURLWS_NOAUTOPONG);
    SET(CURLOPT_TIMEOUT_MS, (long)timeout);
    SET(CURLOPT_CONNECTTIMEOUT_MS, (long)timeout);
    SET(CURLOPT_FOLLOWLOCATION, 0L);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_SSL_VERIFYPEER, 1L);
    SET(CURLOPT_SSL_VERIFYHOST, 2L);
    SET(CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
    SET(CURLOPT_HEADERFUNCTION, responses_http_header);
    SET(CURLOPT_HEADERDATA, t);
    if (p->loopback)
        SET(CURLOPT_PROXY, "");
#undef SET
    if (curl_multi_add_handle(ws->multi, ws->easy) != CURLM_OK)
        goto failed;
    ws->added = true;
    return ws;
failed:
    free_socket(ws);
    return NULL;
}

ah_code responses_ws_start(responses_request *t) {
    struct responses_ws *ws = take_idle(t);
    if (ws && ws->resume_tools) {
        if (!t->full_websocket_input)
            ws->send = responses_ws_encode(t->json, &ws->previous, ws->steering.input,
                                            &ws->input, &ws->continued, &ws->send_size);
        if (!ws->send) {
            free_socket(ws);
            ws = NULL;
        }
    }
    t->websocket_reused = ws != NULL;
    if (!ws)
        ws = connect_socket(t);
    if (!ws)
        return ah_fail(&t->error, AH_IO, "cannot open provider WebSocket");
    const responses_ws_context *prior = t->full_websocket_input ? NULL : &ws->previous;
    if (!ws->send)
        ws->send = responses_ws_encode(t->json, prior, NULL, &ws->input, &ws->continued,
                                       &ws->send_size);
    if (!ws->send) {
        free_socket(ws);
        return ah_fail(&t->error, AH_IO, "cannot encode provider WebSocket request");
    }
    if (ws->resume_tools) {
        /* Confirm the parked peer can receive before sending committed tool outputs. */
        ws->probe_deadline = responses_after(ah_now_ms(), WS_REUSE_PROBE_MS);
        responses_ws_io_probe(ws->io);
    }
    t->ws = ws;
    t->auth_generation = ws->credential;
    t->connection = ws->connection;
    return AH_OK;
}

CURL *responses_ws_easy(responses_request *t) {
    return t->ws->easy;
}

static size_t message_limit(CURL *easy) {
    struct curl_header *header;
    if (curl_easy_header(easy, "x-codex-websocket-max-message-bytes", 0, CURLH_HEADER | CURLH_1XX,
                         -1, &header) ||
        header->amount != 1 || *header->value < '1' || *header->value > '9')
        return 0;
    errno = 0;
    char *end;
    unsigned long long length = strtoull(header->value, &end, 10);
    return !errno && !*end && length <= SIZE_MAX ? (size_t)length : 0;
}

static bool connected(responses_request *t, CURLcode code) {
    struct responses_ws *ws = t->ws;
    if (code != CURLE_OK || responses_check_stop(t)) {
        responses_attempt_finish(t, code);
        return false;
    }
    curl_socket_t socket;
    if (t->status != 101 ||
        curl_easy_getinfo(ws->easy, CURLINFO_ACTIVESOCKET, &socket) != CURLE_OK ||
        socket == CURL_SOCKET_BAD) {
        responses_attempt_finish(t, CURLE_GOT_NOTHING);
        return false;
    }
    ws->max_send = message_limit(ws->easy);
    curl_easy_setopt(ws->easy, CURLOPT_HEADERFUNCTION, NULL);
    curl_easy_setopt(ws->easy, CURLOPT_HEADERDATA, NULL);
    ws->io = responses_ws_io_open(t->provider, ws->easy, socket);
    if (!ws->io) {
        t->failure = RESP_LOCAL;
        ah_fail(&t->error, AH_IO, "cannot start provider WebSocket I/O pump");
        responses_attempt_finish(t, CURLE_FAILED_INIT);
        return false;
    }
    ws->expires_at = responses_after(ah_now_ms(), WS_SESSION_MAX_MS);
    ws->ready = true;
    t->connect_deadline = 0;
    return true;
}

static bool upgrade(responses_request *t) {
    CURLM *multi = t->ws->multi;
    int running, pending;
    if (curl_multi_perform(multi, &running) != CURLM_OK) {
        responses_attempt_finish(t, CURLE_FAILED_INIT);
        return false;
    }
    CURLMsg *message = curl_multi_info_read(multi, &pending);
    return message && message->msg == CURLMSG_DONE && connected(t, message->data.result);
}

static bool retry_advice(responses_request *t, yyjson_val *headers) {
    uint64_t max_delay = 0;
    bool found = false;
    size_t index, count;
    yyjson_val *key, *value;
    yyjson_obj_foreach(headers, index, count, key, value) {
        const char *text = yyjson_get_str(value);
        uint64_t delay;
        if (yyjson_get_len(key) == 11 && !strncasecmp(yyjson_get_str(key), "Retry-After", 11) &&
            text && !memchr(text, '\n', yyjson_get_len(value)) &&
            !memchr(text, '\r', yyjson_get_len(value)) &&
            responses_retry_after(text, yyjson_get_len(value), &delay)) {
            if (delay > max_delay)
                max_delay = delay;
            found = true;
        }
    }
    if (found) {
        t->retry_after = responses_after(ah_now_ms(), max_delay);
        t->retry_after_set = true;
    }
    return found;
}

static ah_code read_event(responses_request *t, yyjson_val *root) {
    struct responses_ws *ws = t->ws;
    if (ah_json_check(root, &t->error))
        return t->error.code;
    yyjson_val *type = yyjson_obj_get(root, "type");
    if (!yyjson_is_str(type))
        return ah_fail(&t->error, AH_PROTOCOL, "WebSocket event requires a type");
    if (session_metadata(type))
        return AH_OK;
    if (ws->resume_tools) {
        if (responses_ws_pending_event(&ws->steering, root))
            return AH_OK;
        if (yyjson_equals_str(type, "response.steer.failed") &&
            yyjson_equals_str(yyjson_obj_get(yyjson_obj_get(root, "steer"), "id"), ws->steering.id)) {
            t->websocket_rebuild = true;
            t->failure = RESP_WS_CONTEXT;
            return ah_fail(&t->error, AH_PROTOCOL, "provider dropped queued steer before tool continuation");
        }
        if (yyjson_equals_str(type, "response.created")) {
            ws->resume_tools = false;
            responses_ws_steer_clear(&ws->steering);
        }
    } else {
        bool handled = false;
        ah_code status = responses_ws_steer_event(t, &ws->steering, ws->response_id, root, &handled);
        if (status || handled)
            return status;
    }
    if (yyjson_equals_str(type, "error") && !*ws->response_id) {
        yyjson_val *error = yyjson_obj_get(root, "error");
        yyjson_val *code = yyjson_obj_get(error, "code");
        if ((ws->continued && yyjson_equals_str(code, "previous_response_not_found")) ||
            (!t->full_websocket_input && yyjson_equals_str(code, "websocket_connection_limit_reached"))) {
            t->websocket_rebuild = true;
            t->failure = RESP_WS_CONTEXT;
            return ah_fail(&t->error, AH_PROTOCOL, "provider WebSocket continuation expired");
        }
        responses_failure failure = ah_stream_failure(root);
        yyjson_val *status_value = yyjson_obj_get(root, "status");
        int status = yyjson_get_int(status_value ? status_value : yyjson_obj_get(root, "status_code"));
        if (status == 401 || status == 403 || status == 429)
            failure = status == 401 ? RESP_AUTH : status == 403 ? RESP_PERMISSION :
                      failure == RESP_NONE ? RESP_RATE : failure;
        if (failure != RESP_NONE) {
            t->failure = failure;
            t->websocket_rejected = true;
            if (failure == RESP_RATE && !retry_advice(t, yyjson_obj_get(error, "headers")))
                retry_advice(t, yyjson_obj_get(root, "headers"));
            return ah_stream_failure_error(failure, &t->error);
        }
    }
    yyjson_val *response = yyjson_obj_get(root, "response");
    yyjson_val *id = response ? yyjson_obj_get(response, "id") : yyjson_obj_get(root, "response_id");
    if (yyjson_equals_str(type, "response.created")) {
        const char *value = yyjson_get_str(id);
        size_t length = yyjson_get_len(id);
        if (!value || !length || length >= sizeof(ws->response_id) || memchr(value, 0, length) ||
            *ws->response_id ||
            (*ws->steering.parent && yyjson_equals_str(id, ws->steering.parent)))
            return ah_fail(&t->error, AH_PROTOCOL, "invalid WebSocket response identity");
        memcpy(ws->response_id, value, length + 1);
        if (t->steer && t->steer->successor)
            ws->input.valid = false;
    } else if ((id && *ws->response_id && !yyjson_equals_str(id, ws->response_id)) ||
               (yyjson_equals_str(type, "response.completed") &&
                (!*ws->response_id || !yyjson_equals_str(id, ws->response_id))) ||
               (!*ws->response_id && !yyjson_equals_str(type, "error") &&
                !yyjson_equals_str(type, "response.queued"))) {
        return ah_fail(&t->error, AH_PROTOCOL, "unexpected WebSocket response identity");
    }
    ah_code result = ah_stream_apply(&t->stream, root);
    if (result)
        t->error = t->stream.error;
    else if (t->stream.terminal) {
        if (*ws->session)
            responses_ws_remember(&ws->previous, &ws->input, ws->response_id, response);
        responses_ws_steer_terminal(t, &ws->steering, ws->response_id);
    }
    return result;
}

static ah_code consume(responses_request *t, const char *bytes, size_t length,
                       const struct curl_ws_frame *frame) {
    struct responses_ws *ws = t->ws;
    if (frame->flags & CURLWS_CLOSE) {
        unsigned close_code = length >= 2 ? (unsigned char)bytes[0] * 256U + (unsigned char)bytes[1] : 0;
        if (close_code == 1009 && !ws->seen_text) {
            t->websocket_fallback = true;
            t->failure = RESP_WS_SIZE;
            return ah_fail(&t->error, AH_LIMIT, "provider rejected WebSocket message size");
        }
        t->failure = RESP_TRANSPORT;
        return ah_fail(&t->error, AH_NETWORK, "provider WebSocket closed before completion");
    }
    if (!(frame->flags & CURLWS_TEXT))
        return ah_fail(&t->error, AH_PROTOCOL, "expected WebSocket text message");
    ws->seen_text = true;
    size_t limit = t->provider->config.max_event_bytes;
    if (length > t->limit - t->received || ws->message_size > limit ||
        length > limit - ws->message_size ||
        (uint64_t)frame->bytesleft > limit - ws->message_size - length)
        return ah_fail(&t->error, AH_LIMIT, "WebSocket response byte limit exceeded");
    if (!append_message(ws, bytes, length, limit))
        return ah_fail(&t->error, AH_IO, "cannot allocate WebSocket message");
    t->received += length;
    t->measurement.metrics.response_bytes += length;
    responses_stream_activity(t);
    if (frame->bytesleft || (frame->flags & CURLWS_CONT))
        return AH_OK;
    yyjson_doc *event = ws->message_size ? yyjson_read(ws->message, ws->message_size, 0) : NULL;
    ws->message_size = 0;
    if (!event)
        return ah_fail(&t->error, AH_PROTOCOL, "invalid WebSocket event JSON");
    ah_code code = read_event(t, yyjson_doc_get_root(event));
    yyjson_doc_free(event);
    return code;
}

void responses_ws_pump(responses_request *t) {
    struct responses_ws *ws = t->ws;
    if (responses_check_stop(t)) {
        responses_attempt_finish(t, CURLE_ABORTED_BY_CALLBACK);
        return;
    }
    if (!ws->ready && !upgrade(t))
        return;
    t->status = 101;
    t->measurement.metrics.request_bytes = ws->send_size;
    if (ws->max_send && ws->send_size > ws->max_send) {
        t->websocket_fallback = true;
        t->failure = RESP_WS_SIZE;
        ah_fail(&t->error, AH_LIMIT, "request exceeds provider WebSocket message limit");
        responses_attempt_finish(t, CURLE_OK);
        return;
    }
    if (ws->probe_deadline) {
        CURLcode code = responses_ws_io_probe_result(ws->io);
        if (code == CURLE_AGAIN && ah_now_ms() < ws->probe_deadline)
            return;
        if (code != CURLE_OK) {
            t->websocket_rebuild = true;
            if (code == CURLE_AGAIN) {
                t->failure = RESP_TRANSPORT;
                ah_fail(&t->error, AH_NETWORK, "parked provider WebSocket did not answer Ping");
                code = CURLE_OPERATION_TIMEDOUT;
            }
            responses_attempt_finish(t, code);
            return;
        }
        ws->probe_deadline = 0;
    }
    if (!t->sent) {
        size_t sent = 0;
        CURLcode code = responses_ws_io_send(ws->io, ws->send + ws->send_offset,
                                             ws->send_size - ws->send_offset, &sent,
                                             &t->websocket_submitted);
        ws->send_offset += sent;
        t->uploaded_bytes += sent;
        if (code != CURLE_OK && code != CURLE_AGAIN) {
            if (t->websocket_reused && !t->websocket_submitted)
                t->websocket_rebuild = true;
            responses_attempt_finish(t, code);
            return;
        }
        if (code == CURLE_AGAIN)
            return;
        responses_request_sent(t);
    }
    if (responses_ws_send_steer(t, &ws->steering, ws->response_id, ws->io)) {
        responses_attempt_finish(t, CURLE_ABORTED_BY_CALLBACK);
        return;
    }
    for (unsigned i = 0; i < 16; ++i) {
        char bytes[AH_WS_IO_CHUNK];
        size_t received = 0;
        struct curl_ws_frame frame;
        CURLcode code = responses_ws_io_recv(ws->io, bytes, &received, &frame);
        if (code == CURLE_AGAIN)
            return;
        /* A peer acknowledgement can be queued in the same pump cycle as the write. */
        if (code == CURLE_OK && ws->steering.frame && !ws->steering.write_done &&
            responses_ws_send_steer(t, &ws->steering, ws->response_id, ws->io)) {
            responses_attempt_finish(t, CURLE_ABORTED_BY_CALLBACK);
            return;
        }
        if (code != CURLE_OK || consume(t, bytes, received, &frame) ||
            (t->stream.terminal && !responses_ws_steer_holds(t, &ws->steering))) {
            responses_attempt_finish(t, code);
            return;
        }
        if (responses_check_stop(t)) {
            responses_attempt_finish(t, CURLE_ABORTED_BY_CALLBACK);
            return;
        }
        if (responses_ws_send_steer(t, &ws->steering, ws->response_id, ws->io)) {
            responses_attempt_finish(t, CURLE_ABORTED_BY_CALLBACK);
            return;
        }
    }
}

unsigned responses_ws_pollfds(ah_responses *p, struct curl_waitfd *fds, unsigned cap) {
    unsigned count = 0;
    for (responses_request *t = p->requests; t && count < cap; t = t->next) {
        if (!t->active || !t->ws || t->ws->ready)
            continue;
        fd_set read, write, error;
        int maxfd;
        FD_ZERO(&read);
        FD_ZERO(&write);
        FD_ZERO(&error);
        if (curl_multi_fdset(t->ws->multi, &read, &write, &error, &maxfd) != CURLM_OK)
            continue;
        for (int fd = 0; fd <= maxfd && fd < FD_SETSIZE && count < cap; ++fd) {
            short events = (FD_ISSET(fd, &read) ? CURL_WAIT_POLLIN : 0) |
                           (FD_ISSET(fd, &write) ? CURL_WAIT_POLLOUT : 0) |
                           (FD_ISSET(fd, &error) ? CURL_WAIT_POLLPRI : 0);
            if (events)
                fds[count++] = (struct curl_waitfd){.fd = fd, .events = events};
        }
    }
    return count;
}

void responses_ws_classify(responses_request *t, CURLcode code) {
    struct responses_ws *ws = t->ws;
    if (t->error.code || t->status >= 300) {
        responses_classify(t, code);
        if (!ws->ready && t->failure == RESP_CONNECT_TIMEOUT)
            t->websocket_fallback = true;
        if (!ws->ready && t->failure == RESP_HTTP &&
            (t->status == 404 || t->status == 405 || t->status == 426 || t->status == 501))
            t->websocket_fallback = true;
    } else if (code == CURLE_OK && ws->ready && t->sent && t->stream.terminal) {
        if (ah_stream_finish(&t->stream, &t->response)) {
            t->error = t->stream.error;
            t->failure = RESP_PROTOCOL;
        }
    } else if (code == CURLE_OPERATION_TIMEDOUT && !ws->ready) {
        t->failure = RESP_CONNECT_TIMEOUT;
        ah_fail(&t->error, AH_LIMIT, "provider WebSocket handshake timed out");
        t->websocket_fallback = true;
    } else if (code == CURLE_SSL_CONNECT_ERROR && !ws->ready && !t->status) {
        /* A dropped TLS handshake can be retried before sending a model request.
         * Certificate and verification errors have separate terminal curl codes. */
        t->failure = RESP_TRANSPORT;
        ah_fail(&t->error, AH_NETWORK, "provider WebSocket TLS handshake failed");
        t->websocket_fallback = t->websocket_tls_retried ||
                                t->retries >= t->provider->config.max_retries;
        t->websocket_tls_retried = true;
    } else {
        responses_classify(t, code ? code : CURLE_GOT_NOTHING);
        if (!ws->ready && (t->status == 200 || code == CURLE_UNSUPPORTED_PROTOCOL ||
                           code == CURLE_WEIRD_SERVER_REPLY ||
                           responses_transient(t->failure, code)))
            t->websocket_fallback = true;
    }
    if (t->failure == RESP_CANCELLED || t->failure == RESP_DEADLINE) {
        t->websocket_fallback = t->websocket_rebuild = false;
    } else if (t->websocket_fallback && !ws->ready) {
        t->provider->websocket_unavailable_until = responses_after(ah_now_ms(), WS_UPGRADE_COOLDOWN_MS);
        if (t->failure != RESP_CONNECT_TIMEOUT)
            t->failure = RESP_WS_UNAVAILABLE;
    }
    if (t->error.code && t->steer && t->steer->in_flight &&
        !t->steer->hard_cancelled(t->steer->context) && ah_now_ms() < t->deadline) {
        t->steer->in_flight = false;
        t->steer->applied = false;
        t->failure = RESP_CANCELLED;
        ah_fail(&t->error, AH_CANCELLED, "native steer could not complete on this connection");
        t->websocket_fallback = t->websocket_rebuild = false;
    }
}

void responses_ws_pause_io(responses_request *t) {
    t->uploaded_bytes += responses_ws_io_pause(t->ws->io, &t->websocket_submitted);
}

void responses_ws_finish(responses_request *t) {
    ah_responses *p = t->provider;
    struct responses_ws *ws = t->ws;
    t->ws = NULL;
    if (t->error.code || !t->response || !*ws->session || ws->credential != p->auth_generation ||
        ah_now_ms() >= ws->expires_at || responses_ws_io_status(ws->io)) {
        free_socket(ws);
        return;
    }
    ws->resume_tools = t->steer && t->steer->awaiting_tools;
    if (!ws->resume_tools)
        responses_ws_steer_clear(&ws->steering);
    ws->response_id[0] = 0;
    ws->seen_text = false;
    ws->send_offset = ws->send_size = 0;
    free(ws->send);
    free(ws->message);
    ws->send = ws->message = NULL;
    ws->message_size = ws->message_cap = 0;
    ws->next = p->idle_websockets;
    p->idle_websockets = ws;
    struct responses_ws **link = &ws->next;
    for (unsigned count = 1; *link;) {
        struct responses_ws *idle = *link;
        if (!strcmp(idle->session, ws->session) || count >= WS_IDLE_CONNECTIONS) {
            *link = idle->next;
            free_socket(idle);
        } else {
            ++count;
            link = &idle->next;
        }
    }
    responses_ws_io_resume(ws->io, ws->expires_at);
}
#else
bool ah_responses_websocket_available(void) { return false; }
bool responses_ws_selected(const responses_request *t) { (void)t; return false; }
bool responses_ws_steer_pending(responses_request *t) { (void)t; return false; }
ah_code responses_ws_start(responses_request *t) { (void)t; return AH_INVALID; }
CURL *responses_ws_easy(responses_request *t) { (void)t; return NULL; }
void responses_ws_pause_io(responses_request *t) { (void)t; }
void responses_ws_pump(responses_request *t) { (void)t; }
unsigned responses_ws_pollfds(ah_responses *p, struct curl_waitfd *fds, unsigned cap) {
    (void)p; (void)fds; (void)cap; return 0;
}
void responses_ws_classify(responses_request *t, CURLcode code) { (void)t; (void)code; }
void responses_ws_finish(responses_request *t) { (void)t; }
void responses_ws_clear(ah_responses *p) { (void)p; }
void responses_ws_shutdown(ah_responses *p) { (void)p; }
#endif
