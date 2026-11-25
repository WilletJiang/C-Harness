#ifndef AH_RESPONSE_STREAM_H
#define AH_RESPONSE_STREAM_H
#include "ah/ah.h"

/* Private, transport-independent incremental Responses SSE decoder. */
typedef enum { AH_STREAM_CONTENT, AH_STREAM_OUTPUT_ITEM, AH_STREAM_TERMINAL } ah_stream_phase;
typedef void (*ah_stream_phase_sink)(void *context, ah_stream_phase phase);
typedef enum {
    RESP_NONE, RESP_AUTH, RESP_PERMISSION, RESP_QUOTA, RESP_SUBSCRIPTION_LIMIT,
    RESP_SUBSCRIPTION_UNAVAILABLE, RESP_RATE, RESP_SERVER, RESP_CONTEXT, RESP_OUTPUT_LIMIT,
    RESP_HTTP, RESP_PROTOCOL, RESP_TRANSPORT, RESP_TLS, RESP_CONNECT_TIMEOUT,
    RESP_FIRST_OUTPUT_TIMEOUT, RESP_IDLE_TIMEOUT, RESP_DRAIN_TIMEOUT, RESP_STREAM_INTERRUPTED,
    RESP_DEADLINE, RESP_CANCELLED, RESP_LIMIT, RESP_REFRESH_FAILED, RESP_REFRESH_UNAVAILABLE,
    RESP_LOCAL, RESP_WS_UNAVAILABLE, RESP_WS_SIZE, RESP_WS_CONTEXT, RESP_TLS_CERTIFICATE,
    RESP_TLS_DISCONNECTED, RESP_PROXY_DISCONNECTED, RESP_PROXY_REJECTED
} responses_failure;
typedef struct {
    struct ah_stream_items *items;
    char *line, *data, *response;
    size_t line_len, line_cap, data_len, data_cap, frame_bytes, total_bytes;
    size_t event_limit, response_limit;
    unsigned utf8_left, utf8_min, utf8_max;
    bool after_cr, terminal, observed_output;
    ah_delta_sink delta;
    void *delta_context;
    ah_stream_phase_sink phase;
    void *phase_context;
    ah_error error;
    responses_failure failure;
} ah_stream;

void ah_stream_init(ah_stream *stream, size_t event_limit, size_t response_limit,
                    ah_delta_sink delta, void *context);
ah_code ah_stream_feed(ah_stream *stream, const char *bytes, size_t length);
ah_code ah_stream_finish(ah_stream *stream, char **response);
void ah_stream_destroy(ah_stream *stream);
struct yyjson_val;
ah_code ah_stream_apply(ah_stream *stream, struct yyjson_val *event);
responses_failure ah_stream_failure(struct yyjson_val *root);
ah_code ah_stream_failure_error(responses_failure failure, ah_error *error);
ah_code ah_stream_item(ah_stream *stream, struct yyjson_val *event, bool done);
ah_code ah_stream_response(ah_stream *stream, struct yyjson_val *response);
bool ah_stream_content(struct yyjson_val *item);
void ah_stream_items_destroy(ah_stream *stream);
#endif
