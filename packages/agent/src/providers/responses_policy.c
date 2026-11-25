#include "responses_internal.h"
#include <errno.h>
#include <strings.h>
#include <time.h>

uint64_t responses_after(uint64_t now, uint64_t delay) {
    return delay > UINT64_MAX - now ? UINT64_MAX : now + delay;
}

static bool http_date(const char *value, size_t length) {
    const char *comma = memchr(value, ',', length);
    if (!comma)
        return length == 24 && value[3] == ' ' && value[7] == ' ' &&
               value[10] == ' ' && value[13] == ':' && value[16] == ':' && value[19] == ' ';
    if (length < 4 || memcmp(value + length - 4, " GMT", 4))
        return false;
    size_t weekday = (size_t)(comma - value);
    if (weekday == 3)
        return length == 29 && value[4] == ' ' && value[7] == ' ' && value[11] == ' ' &&
               value[16] == ' ' && value[19] == ':' && value[22] == ':';
    return weekday >= 6 && weekday <= 9 && length - weekday == 24 &&
           comma[1] == ' ' && comma[4] == '-' && comma[8] == '-' && comma[11] == ' ' &&
           comma[14] == ':' && comma[17] == ':';
}

bool responses_retry_after(const char *value, size_t length, uint64_t *delay) {
    while (length && (*value == ' ' || *value == '\t')) {
        ++value;
        --length;
    }
    while (length && strchr(" \t\r\n", value[length - 1]) && value[length - 1])
        --length;
    if (!length)
        return false;
    if (*value >= '0' && *value <= '9') {
        uint64_t seconds = 0;
        for (size_t i = 0; i < length; ++i) {
            if (value[i] < '0' || value[i] > '9')
                return false;
            unsigned digit = (unsigned)(value[i] - '0');
            seconds = seconds > (UINT64_MAX - digit) / 10 ? UINT64_MAX : seconds * 10 + digit;
        }
        *delay = seconds > UINT64_MAX / 1000 ? UINT64_MAX : seconds * 1000;
        return true;
    }
    char date[64];
    if (length >= sizeof(date) || memchr(value, 0, length) || !http_date(value, length))
        return false;
    memcpy(date, value, length);
    date[length] = 0;
    time_t target = curl_getdate(date, NULL);
    struct timespec now;
    if (target == (time_t)-1 || clock_gettime(CLOCK_REALTIME, &now))
        return false;
    double seconds = difftime(target, now.tv_sec);
    *delay = seconds <= 0 ? 0 : (uint64_t)(seconds * 1000) - (uint64_t)now.tv_nsec / 1000000;
    return true;
}

static responses_failure http_failure(responses_request *t) {
    if (t->status < 400)
        return RESP_HTTP;
    if (t->status == 401)
        return RESP_AUTH;
    if (t->status == 403)
        return RESP_PERMISSION;
    yyjson_doc *doc = t->error_used && t->error_used == t->received
                          ? yyjson_read(t->error_body, t->error_used, 0)
                          : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    responses_failure failure = root && ah_json_check(root, NULL) == AH_OK
                                    ? ah_stream_failure(root) : RESP_NONE;
    yyjson_doc_free(doc);
    if (failure != RESP_NONE && failure != RESP_AUTH && failure != RESP_RATE && failure != RESP_SERVER)
        return failure;
    responses_failure fallback;
    switch (t->status) {
    case 429: fallback = RESP_RATE; break;
    case 408: case 500: case 502: case 503: case 504: case 529: fallback = RESP_SERVER; break;
    default: return RESP_HTTP;
    }
    return failure == RESP_RATE || failure == RESP_SERVER ? failure : fallback;
}

static responses_failure transport_failure(const responses_request *t, CURLcode result) {
    switch (result) {
    case CURLE_PEER_FAILED_VERIFICATION: case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CACERT_BADFILE: case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
    case CURLE_SSL_CRL_BADFILE: case CURLE_SSL_ISSUER_ERROR: case CURLE_SSL_INVALIDCERTSTATUS:
        return RESP_TLS_CERTIFICATE;
    default:
        break;
    }
    if (!t->websocket && !t->status && t->proxy_connect_status >= 300)
        return RESP_PROXY_REJECTED;
    if (t->preupload && !t->proxy_connect_status &&
        (strstr(t->curl_detail, "Proxy CONNECT aborted") ||
         (t->proxy_used && (result == CURLE_RECV_ERROR || result == CURLE_SEND_ERROR ||
                            result == CURLE_GOT_NOTHING))))
        return RESP_PROXY_DISCONNECTED;
    if (result != CURLE_SSL_CONNECT_ERROR)
        return RESP_TRANSPORT;
    /* libcurl's generic TLS setup error also covers alerts and protocol errors.
     * Only treat an evidenced socket interruption as transient. */
    if (t->preupload &&
        (t->socket_errno == ECONNRESET || t->socket_errno == ECONNABORTED ||
         t->socket_errno == EPIPE || strcasestr(t->curl_detail, "unexpected eof") ||
         strstr(t->curl_detail, "TLS connection was non-properly terminated") ||
         (!t->socket_errno && strstr(t->curl_detail, "SSL_ERROR_SYSCALL"))))
        return RESP_TLS_DISCONNECTED;
    return RESP_TLS;
}

bool responses_preupload_disconnect(responses_failure failure) {
    return failure == RESP_PROXY_DISCONNECTED || failure == RESP_TLS_DISCONNECTED;
}

void responses_classify(responses_request *t, CURLcode result) {
    if (t->status >= 300 && t->header_complete &&
        (!t->error.code || t->failure == RESP_FIRST_OUTPUT_TIMEOUT)) {
        t->failure = http_failure(t);
        if (t->failure == RESP_HTTP)
            ah_fail(&t->error, AH_PROTOCOL, "provider HTTP %ld", t->status);
        else
            ah_stream_failure_error(t->failure, &t->error);
    } else if (t->error.code != AH_OK) {
        if (t->failure == RESP_NONE)
            t->failure = t->stream.failure ? t->stream.failure :
                         t->error.code == AH_CANCELLED ? RESP_CANCELLED :
                         t->error.code == AH_PROTOCOL ? RESP_PROTOCOL :
                         t->error.code == AH_LIMIT ? RESP_LIMIT : RESP_LOCAL;
    } else if (result == CURLE_OPERATION_TIMEDOUT) {
        curl_off_t pretransfer = 0;
        CURL *easy = t->websocket ? responses_ws_easy(t) : t->easy;
        curl_easy_getinfo(easy, CURLINFO_PRETRANSFER_TIME_T, &pretransfer);
        bool connection = !t->sent && !t->status && !pretransfer &&
                          t->provider->config.connect_timeout_ms && ah_now_ms() < t->deadline;
        t->failure = connection ? RESP_CONNECT_TIMEOUT : RESP_DEADLINE;
        ah_fail(&t->error, AH_LIMIT, "%s", connection ? "provider connection timeout" :
                                                       "provider deadline exceeded");
    } else if (result != CURLE_OK) {
        t->failure = transport_failure(t, result);
        switch (t->failure) {
        case RESP_PROXY_REJECTED:
            ah_fail(&t->error, AH_NETWORK, "proxy CONNECT rejected (HTTP %ld)", t->proxy_connect_status);
            break;
        case RESP_PROXY_DISCONNECTED:
            ah_fail(&t->error, AH_NETWORK, "proxy disconnected during CONNECT");
            break;
        case RESP_TLS_DISCONNECTED:
            ah_fail(&t->error, AH_NETWORK, "provider TLS handshake disconnected before upload");
            break;
        case RESP_TLS_CERTIFICATE:
            ah_fail(&t->error, AH_NETWORK, "TLS certificate verification or configuration failed");
            break;
        default:
            ah_fail(&t->error, AH_NETWORK, "provider transport failure (%d): %s",
                    (int)result, curl_easy_strerror(result));
        }
    } else if (t->status < 200 || t->status >= 300 ||
               (t->content_type_seen && !t->content_type)) {
        t->failure = RESP_PROTOCOL;
        ah_fail(&t->error, AH_PROTOCOL, "expected text/event-stream response");
    } else if (t->content_type && !t->stream.observed_output && !t->stream.terminal &&
               !t->stream.utf8_left && !t->stream.line_len && !t->stream.data_len) {
        t->failure = RESP_STREAM_INTERRUPTED;
        ah_fail(&t->error, AH_NETWORK, "provider stream ended before output");
    } else if (ah_stream_finish(&t->stream, &t->response)) {
        t->error = t->stream.error;
        t->failure = t->stream.failure ? t->stream.failure : RESP_PROTOCOL;
    }
}

bool responses_transient(responses_failure failure, CURLcode result) {
    switch (failure) {
    case RESP_RATE: case RESP_SERVER: case RESP_CONNECT_TIMEOUT: case RESP_FIRST_OUTPUT_TIMEOUT:
    case RESP_STREAM_INTERRUPTED: case RESP_TLS_DISCONNECTED: case RESP_PROXY_DISCONNECTED:
        return true;
    case RESP_TRANSPORT:
        return result == CURLE_COULDNT_CONNECT || result == CURLE_COULDNT_RESOLVE_HOST ||
               result == CURLE_COULDNT_RESOLVE_PROXY || result == CURLE_SEND_ERROR ||
               result == CURLE_RECV_ERROR || result == CURLE_GOT_NOTHING ||
               result == CURLE_SSL_CONNECT_ERROR ||
               result == CURLE_PARTIAL_FILE || result == CURLE_HTTP2 || result == CURLE_HTTP2_STREAM;
    default: return false;
    }
}

void responses_state(responses_request *t, const char *phase, uint64_t delay) {
    ah_responses *p = t->provider;
    if (!p->config.state)
        return;
    static const char *const reasons[] = {
        [RESP_AUTH] = "authentication", [RESP_PERMISSION] = "permission",
        [RESP_QUOTA] = "quota", [RESP_SUBSCRIPTION_LIMIT] = "subscription_limit",
        [RESP_SUBSCRIPTION_UNAVAILABLE] = "subscription_unavailable", [RESP_RATE] = "rate_limit",
        [RESP_SERVER] = "server", [RESP_CONTEXT] = "context_limit",
        [RESP_OUTPUT_LIMIT] = "output_limit", [RESP_HTTP] = "http", [RESP_PROTOCOL] = "protocol",
        [RESP_TRANSPORT] = "transport", [RESP_TLS] = "tls",
        [RESP_CONNECT_TIMEOUT] = "connect_timeout", [RESP_FIRST_OUTPUT_TIMEOUT] = "first_output_timeout",
        [RESP_IDLE_TIMEOUT] = "stream_idle_timeout", [RESP_DRAIN_TIMEOUT] = "drain_timeout",
        [RESP_STREAM_INTERRUPTED] = "stream_interrupted", [RESP_DEADLINE] = "deadline",
        [RESP_CANCELLED] = "cancelled", [RESP_LIMIT] = "local_limit",
        [RESP_REFRESH_FAILED] = "refresh_failed", [RESP_REFRESH_UNAVAILABLE] = "refresh_unavailable",
        [RESP_LOCAL] = "local",
        [RESP_WS_UNAVAILABLE] = "websocket_unavailable", [RESP_WS_SIZE] = "websocket_size",
        [RESP_WS_CONTEXT] = "websocket_context",
        [RESP_TLS_CERTIFICATE] = "tls_certificate", [RESP_TLS_DISCONNECTED] = "tls_handshake_disconnected",
        [RESP_PROXY_DISCONNECTED] = "proxy_connect_disconnected", [RESP_PROXY_REJECTED] = "proxy_connect_rejected",
    };
    ah_responses_state state = {.session_id = *t->session ? t->session : NULL,
                                .turn = t->turn, .attempt = t->attempt, .phase = phase,
                                .reason = reasons[t->failure], .http_status = t->status,
                                .retry_in_ms = delay,
                                .uploaded_bytes = t->uploaded_bytes,
                                .connection = t->connection,
                                .transport = t->websocket ? "websocket" : "http",
                                .proxy_connect_status = t->proxy_connect_status,
                                .curl_code = (int)t->curl_code, .proxy_used = t->proxy_used};
    p->config.state(p->config.state_context, &state);
}

void responses_settle(responses_request *t) {
    t->waiting_auth = false;
    t->done = true;
    responses_state(t, t->error.code == AH_CANCELLED ? "cancelled" :
                       t->error.code ? "failed" : "completed", 0);
}
