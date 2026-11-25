#ifndef AH_RESPONSES_INTERNAL_H
#define AH_RESPONSES_INTERNAL_H
#include "ah/responses.h"
#include "../internal.h"
#include "stream.h"
#include "turn.h"
#include <curl/curl.h>
#define AH_RESPONSES_OPENAI_ENDPOINT "https://api.openai.com/v1/responses"

/* Bounded per-pool cache; eviction only loses affinity. A late response must
 * still match its slot generation before it can publish a header. */
typedef struct {
    char session[AH_ID_CAP];
    unsigned turn;
    uint64_t generation;
    char *header;
} responses_route;

struct ah_responses {
    ah_responses_config config;
    char *endpoint, *token;
    CURLM *multi;
    responses_route routes[32];
    size_t next_route;
    struct responses_request *requests;
    struct responses_refresh *refresh;
    struct responses_ws *idle_websockets;
    struct responses_ws_io_pool *websocket_io;
    uint64_t auth_generation;
    uint64_t next_websocket;
    uint64_t websocket_unavailable_until;
    bool loopback, dispatching, websocket_available;
};

typedef struct {
    uint64_t started;
    bool curl_started;
    ah_responses_metrics metrics;
} responses_measurement;

typedef struct responses_request {
    struct responses_request *next;
    ah_responses *provider;
    CURL *easy;
    struct responses_ws *ws;
    responses_route *route;
    uint64_t route_generation;
    char *candidate_route;
    bool route_header_seen, route_sent;
    struct curl_slist *headers;
    const char *json;
    char session[AH_ID_CAP], session_header[525];
    char *response;
    ah_delta_sink delta;
    void *delta_context;
    ah_cancel_check cancelled;
    void *cancel_context;
    responses_turn_steer *steer;
    ah_stream stream;
    responses_measurement measurement;
    uint64_t deadline, connect_deadline, phase_deadline, retry_at, retry_after, auth_generation;
    uint64_t connection;
    long status, proxy_connect_status, socket_errno;
    CURLcode curl_code;
    char curl_detail[CURL_ERROR_SIZE];
    size_t received, header_bytes, limit, request_bytes, uploaded_bytes;
    char *error_body;
    size_t error_used;
    unsigned attempt, retries, preupload_retries, turn;
    responses_failure failure;
    bool measure, active, done;
    bool sent, streaming, draining, retry_after_set, auth_retried, waiting_auth;
    bool content_type, content_type_seen, header_complete;
    bool websocket, use_http, full_websocket_input, websocket_fallback, websocket_rebuild;
    bool websocket_submitted, websocket_rejected, websocket_reused;
    bool websocket_tls_retried;
    bool proxy_used, preupload;
    ah_error error;
} responses_request;

void responses_stamp(ah_responses_time *value, uint64_t started);
void responses_phase(void *context, ah_stream_phase phase);
void responses_observe(ah_responses *provider, CURL *easy, responses_measurement *measurement);
void responses_state(responses_request *request, const char *phase, uint64_t delay);
void responses_settle(responses_request *request);
uint64_t responses_after(uint64_t now, uint64_t delay);
bool responses_retry_after(const char *value, size_t length, uint64_t *delay);
void responses_classify(responses_request *request, CURLcode result);
bool responses_transient(responses_failure failure, CURLcode result);
bool responses_preupload_disconnect(responses_failure failure);
bool responses_auth_wait(responses_request *request);
void responses_auth_poll(ah_responses *provider);
void responses_auth_cancel_unused(ah_responses *provider);
void responses_auth_close(ah_responses *provider);

ah_code responses_check_stop(responses_request *request);
ah_code responses_request_setup(responses_request *, const char *sid, unsigned turn, ah_error *);
void responses_route_bind(responses_request *, const char *sid, unsigned turn);
void responses_route_capture(responses_request *, const char *, size_t);
void responses_route_commit(responses_request *);
ah_code responses_route_apply(responses_request *);
void responses_routes_clear(ah_responses *);
void responses_route_reset(responses_request *);
ah_code responses_attempt_start(responses_request *request);
void responses_attempt_finish(responses_request *request, CURLcode result);
void responses_request_sent(responses_request *);
void responses_stream_restart(responses_request *);
void responses_stream_activity(responses_request *);
size_t responses_http_header(char *, size_t, size_t, void *);
char *responses_bearer_header(const char *);
void responses_free_headers(struct curl_slist *);
void responses_request_free(responses_request *request);

bool responses_ws_selected(const responses_request *);
bool responses_ws_steer_pending(responses_request *);
ah_code responses_ws_start(responses_request *);
void responses_ws_pump(responses_request *);
unsigned responses_ws_pollfds(ah_responses *, struct curl_waitfd *, unsigned);
CURL *responses_ws_easy(responses_request *);
void responses_ws_pause_io(responses_request *);
void responses_ws_classify(responses_request *, CURLcode);
void responses_ws_finish(responses_request *);
void responses_ws_clear(ah_responses *);
void responses_ws_shutdown(ah_responses *);

void responses_wipe(char *value);
bool responses_valid_token(const char *token);
void responses_replace_token(ah_responses *provider, char *token);
ah_code responses_complete(void *context, const char *request, ah_delta_sink delta,
                           void *delta_context, ah_cancel_check cancelled, void *cancel_context,
                           char **response, ah_error *error);
#endif
