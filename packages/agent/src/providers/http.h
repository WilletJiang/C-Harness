#ifndef AH_HTTP_H
#define AH_HTTP_H
#include "../internal.h"
ah_code ah_http_init(ah_error *);
bool ah_http_endpoint(const char *, bool allow_loopback_http, bool *loopback);
#endif
