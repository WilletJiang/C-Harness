#include "cli.h"
#include "internal.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void cli_print(cli_context *cli, const char *json) {
    pthread_mutex_lock(&cli->output);
    puts(json ? json : "null");
    fflush(stdout);
    pthread_mutex_unlock(&cli->output);
}
void cli_observe(void *context, const ah_event *event) {
    cli_context *cli = context;
    if (cli->event_sink)
        cli->event_sink(cli->event_context, event);
    if (!cli->json_events && !cli->rpc)
        return;
    yyjson_mut_doc *doc = ah_mut_doc_new();
    if (!doc)
        return;
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, obj);
    yyjson_mut_obj_add_str(doc, obj, "event", event->type);
    yyjson_mut_obj_add_uint(doc, obj, "seq", event->seq);
    if (event->session_id)
        yyjson_mut_obj_add_str(doc, obj, "session", event->session_id);
    if (event->operation_id)
        yyjson_mut_obj_add_str(doc, obj, "operation", event->operation_id);
    yyjson_doc *payload = event->json ? yyjson_read(event->json, strlen(event->json), 0) : NULL;
    if (payload)
        yyjson_mut_obj_add_val(doc, obj, "data",
                               yyjson_val_mut_copy(doc, yyjson_doc_get_root(payload)));
    char *text = ah_mut_write(doc);
    if (text)
        cli_print(cli, text);
    free(text);
    yyjson_doc_free(payload);
    ah_mut_doc_free(doc);
}

void cli_network_state(void *context, const ah_responses_state *state) {
    cli_context *cli = context;
    if (!cli->event_sink && !cli->json_events && !cli->rpc)
        return;
    char payload[400];
    snprintf(payload, sizeof(payload),
             "{\"phase\":\"%s\",\"attempt\":%u,\"turn\":%u,\"http_status\":%ld,"
             "\"retry_in_ms\":%" PRIu64 ",\"reason\":\"%s\",\"transport\":\"%s\","
             "\"uploaded_bytes\":%zu,\"connection\":%" PRIu64 ",\"proxy_used\":%s,"
             "\"proxy_connect_status\":%ld,\"curl_code\":%d}",
             state->phase, state->attempt, state->turn, state->http_status, state->retry_in_ms,
             state->reason ? state->reason : "", state->transport, state->uploaded_bytes,
             state->connection, state->proxy_used ? "true" : "false",
             state->proxy_connect_status, state->curl_code);
    ah_event event = {.type = "network.state", .session_id = state->session_id, .json = payload};
    cli_observe(cli, &event);
}
