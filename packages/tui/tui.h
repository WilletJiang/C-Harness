#ifndef AH_TUI_H
#define AH_TUI_H
#include "cli.h"
#include "ah/responses.h"

ah_code cli_tui(cli_context *, ah_responses *, const char *database, const char *session,
                const char *model, const char *instructions, const char *endpoint,
                bool endpoint_explicit, bool has_cli_token, const char *token_source,
                bool allow_loopback_http, ah_error *);
#endif
