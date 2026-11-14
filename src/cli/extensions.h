#ifndef AH_CLI_EXTENSIONS_H
#define AH_CLI_EXTENSIONS_H
#include "ah/extensions.h"

typedef enum { CLI_OPTION_UNHANDLED, CLI_OPTION_HANDLED, CLI_OPTION_INVALID } cli_option_status;

cli_option_status cli_extension_option(ah_extensions_config *config, int argc, char **argv,
                                       int *index);
#endif
