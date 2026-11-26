#ifndef AH_APP_SIGNALS_H
#define AH_APP_SIGNALS_H
#include "ah/ah.h"

bool app_signals_start(ah_engine *engine);
void app_signals_set_engine(ah_engine *engine);
int app_shutdown_fd(void);
void app_signals_stop(void);

#endif
