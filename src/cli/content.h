#ifndef AH_CLI_CONTENT_H
#define AH_CLI_CONTENT_H
#include "cli.h"

typedef struct {
    const char *json, *path, *mime;
    bool tree;
} cli_content_options;
cli_option_status cli_content_option(cli_content_options *, int, char **, int *);
bool cli_content_options_valid(const cli_content_options *, const char *, const char *,
                               const char *);
bool cli_content_command(const char *);
ah_code cli_content_execute(ah_engine *, const char *, const char *, const char *,
                            const cli_content_options *, char **, ah_error *);
bool cli_content_rpc(ah_engine *, const char *, yyjson_val *, ah_code *, char **, ah_error *);

#endif
