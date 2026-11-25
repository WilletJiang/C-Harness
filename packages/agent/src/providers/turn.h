#ifndef AH_RESPONSES_TURN_H
#define AH_RESPONSES_TURN_H
#include "ah/async.h"

typedef struct responses_turn_steer {
    void *context;
    const char *(*peek)(void *context, char command[257]);
    ah_cancel_check hard_cancelled;
    char command[257];
    bool in_flight, successor, applied, awaiting_tools;
} responses_turn_steer;

/* Native Responses calls receive the owner's turn identity outside the wire JSON.
 * Other providers keep their ordinary callback contract. */
ah_code responses_complete_in_turn(ah_provider, const char *sid, unsigned turn, const char *json,
                                   ah_delta_sink, void *, ah_cancel_check, void *,
                                   responses_turn_steer *, char **,
                                   ah_error *);
ah_code responses_start_in_turn(ah_async_provider, const char *sid, unsigned turn, const char *json,
                                ah_delta_sink, void *, ah_cancel_check, void *,
                                responses_turn_steer *, void **,
                                ah_error *);
#endif
