#ifndef AH_CLI_POLICIES_H
#define AH_CLI_POLICIES_H
#include "cli.h"
#include "ah/context.h"

typedef struct {
    ah_context_config context;
    size_t context_bytes;
    bool automatic_context, coordinate_writes;
} cli_policy_options;

void cli_policy_init(cli_policy_options *);
cli_option_status cli_policy_option(cli_policy_options *, int, char **, int *);
ah_code cli_policy_enable(ah_engine *, const cli_policy_options *, const char *, ah_error *);
bool cli_policy_command(const char *);
ah_code cli_policy_execute(ah_engine *, const char *, const char *, char **, ah_error *);
bool cli_policy_rpc(ah_engine *, const char *, yyjson_val *, ah_code *, char **, ah_error *);

#endif
