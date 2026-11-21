#ifndef AH_WORKSPACE_INTERNAL_H
#define AH_WORKSPACE_INTERNAL_H
#include "core.h"
#include "ah/workspace.h"
#include <sys/stat.h>

ah_code workspace_load(ah_engine *, char **, struct stat *, bool *, ah_error *);
ah_code workspace_idle(ah_engine *, ah_error *);
ah_code workspace_open(ah_engine *, int *, ah_error *);
ah_code workspace_paths_overlap(int, const char *, const char *, bool *, ah_error *);
ah_code workspace_path_contains(int, const char *, const char *, bool *, ah_error *);
bool workspace_valid_path(int, const char *);
ah_code workspace_session(ah_engine *, const char *, ah_error *);
#endif
