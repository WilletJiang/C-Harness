#ifndef AH_TUI_H
#define AH_TUI_H
#include "cli.h"
#include "ah/responses.h"
#include <limits.h>

typedef ah_code (*tui_open_engine)(void *, const char *, ah_engine **, ah_local_tools **,
                                  ah_error *);
typedef bool (*tui_visit_database)(const char *, void *);

ah_code tui_state_directory(const char *workspace, char path[PATH_MAX], ah_error *);
ah_code tui_visit_databases(const char *directory, tui_visit_database, void *, ah_error *);
ah_code tui_session_database(const char *directory, const char *session, char path[PATH_MAX],
                             ah_error *);
ah_code tui_open_window(const char *directory, char path[PATH_MAX], tui_open_engine, void *,
                        ah_engine **, ah_local_tools **, ah_error *);

ah_code cli_tui(cli_context *, ah_responses *, const char *database, const char *session,
                const char *model, const char *instructions, const char *endpoint,
                bool endpoint_explicit, bool has_cli_token, const char *token_source,
                bool allow_loopback_http, const char *state_directory,
                tui_open_engine, void *engine_context, ah_error *);
#endif
