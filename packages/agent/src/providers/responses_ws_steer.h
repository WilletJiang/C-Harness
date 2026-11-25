#ifndef AH_RESPONSES_WS_STEER_H
#define AH_RESPONSES_WS_STEER_H
#include "responses_ws_io.h"

typedef struct {
    char parent[257], id[257];
    char *frame, *input;
    size_t length, sent;
    bool write_done, accepted, parent_ended, waiting_tools, rejected;
} responses_ws_steer;

void responses_ws_steer_clear(responses_ws_steer *);
ah_code responses_ws_send_steer(responses_request *, responses_ws_steer *, const char *,
                                 responses_ws_io *);
ah_code responses_ws_steer_event(responses_request *, responses_ws_steer *, char *,
                                 yyjson_val *, bool *);
void responses_ws_steer_terminal(responses_request *, responses_ws_steer *, char *);
bool responses_ws_steer_holds(const responses_request *, const responses_ws_steer *);
bool responses_ws_pending_event(const responses_ws_steer *, yyjson_val *);
#endif
