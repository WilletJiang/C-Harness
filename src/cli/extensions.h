#ifndef AH_CLI_EXTENSIONS_H
#define AH_CLI_EXTENSIONS_H
#include "cli.h"
#include "ah/extensions.h"

cli_option_status cli_extension_option(ah_extensions_config *config, int argc, char **argv,
                                       int *index);
#endif
