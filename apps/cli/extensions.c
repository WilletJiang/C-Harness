#include "extensions.h"
#include <stdio.h>
#include <string.h>

cli_option_status cli_extension_option(ah_extensions_config *config, int argc, char **argv,
                                       int *index) {
    const char *option = argv[*index];
    if (!strcmp(option, "--skills"))
        config->enable_skills = true;
    else if (!strcmp(option, "--mcp"))
        config->enable_mcp = true;
    else if (!strcmp(option, "--plugins"))
        config->enable_plugins = true;
    else if (!strcmp(option, "--codex-home") || !strcmp(option, "--extension-context-bytes")) {
        if (*index + 1 == argc) {
            fprintf(stderr, "Missing value for %s\n", option);
            return CLI_OPTION_INVALID;
        }
        const char *value = argv[++*index];
        if (!strcmp(option, "--codex-home")) {
            config->codex_home = value;
        } else {
            uintmax_t bytes;
            if (!cli_option_number(option, value, 1024, SIZE_MAX, &bytes))
                return CLI_OPTION_INVALID;
            config->max_context_bytes = (size_t)bytes;
        }
    } else {
        return CLI_OPTION_UNHANDLED;
    }
    return CLI_OPTION_HANDLED;
}
