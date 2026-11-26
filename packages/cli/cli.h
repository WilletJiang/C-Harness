#ifndef AH_CLI_H
#define AH_CLI_H
#include "ah/ah.h"
#include "ah/responses.h"
#include "ah/tools.h"
#include "yyjson.h"
#include <pthread.h>
typedef enum { CLI_OPTION_UNHANDLED, CLI_OPTION_HANDLED, CLI_OPTION_INVALID } cli_option_status;
typedef struct {
    ah_engine *engine;
    ah_local_tools *local_tools;
    pthread_mutex_t output;
    bool json_events;
    bool rpc;
    const char *workspace;
    char *owned_database;
    ah_observer event_sink;
    void *event_context;
} cli_context;
void cli_usage(bool all);
bool cli_option_number(const char *, const char *, uintmax_t, uintmax_t, uintmax_t *);
void cli_observe(void *context, const ah_event *event);
void cli_network_state(void *context, const ah_responses_state *state);
void cli_print(cli_context *cli, const char *json);
int cli_rpc(cli_context *cli);
char *cli_load_token(const char *path);
ah_code cli_reload_token(void *path, const char *rejected, uint64_t deadline_ms,
                         ah_cancel_check cancelled, void *cancel_context, char **replacement);
#endif
