#ifndef AH_RESPONSES_TURN_H
#define AH_RESPONSES_TURN_H
#include "ah/async.h"

/* Native Responses calls receive the owner's turn identity outside the wire JSON.
 * Other providers keep their ordinary callback contract. */
ah_code responses_complete_in_turn(ah_provider, const char *sid, unsigned turn, const char *json,
                                   ah_delta_sink, void *, ah_cancel_check, void *, char **,
                                   ah_error *);
ah_code responses_start_in_turn(ah_async_provider, const char *sid, unsigned turn, const char *json,
                                ah_delta_sink, void *, ah_cancel_check, void *, void **,
                                ah_error *);
#endif
