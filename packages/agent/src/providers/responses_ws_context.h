#ifndef AH_RESPONSES_WS_CONTEXT_H
#define AH_RESPONSES_WS_CONTEXT_H
#include "../internal.h"
#include "../hash.h"

typedef struct {
    char id[257], prefix[65], settings[65];
    size_t count;
} responses_ws_context;

typedef struct {
    ah_hash input;
    char settings[65];
    size_t count;
    bool valid;
} responses_ws_input;

char *responses_ws_encode(const char *, const responses_ws_context *, const char *, responses_ws_input *,
                          bool *, size_t *);
void responses_ws_remember(responses_ws_context *, responses_ws_input *, const char *, yyjson_val *);
#endif
