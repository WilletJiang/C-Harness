#ifndef AH_DURABLE_FIXTURE_H
#define AH_DURABLE_FIXTURE_H
#include "ah/ah.h"
#include "yyjson.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DF_OPERATIONS 32

typedef struct {
    ah_engine *engine;
    pthread_t owner;
    const char *tool;
    const char *arguments;
    unsigned batch, models, receipts;
    char operations[DF_OPERATIONS][AH_ID_CAP];
    unsigned started;
    atomic_uint calls, active, peak;
    int ready_fd;
    bool signal_on_start;
    bool crash_after_receipt;
    ah_code reentry_code;
} durable_fixture;

void df_ok(ah_code, const ah_error *);
void df_open(durable_fixture *, const char *database, unsigned workers);
void df_create(durable_fixture *, char session[AH_ID_CAP]);
void df_state(durable_fixture *, const char *session, ah_state);
unsigned df_events(durable_fixture *, const char *session, const char *type);
void df_database(char path[64]);
void df_remove_database(const char *path);
void df_ready(int fd);
void df_kill_child(const char *executable, const char *mode, const char *database,
                   const char *session);
void df_sleep(unsigned milliseconds);
void df_recovery_cases(const char *executable);
int df_recovery_child(int argc, char **argv);
void df_composition_cases(void);
#endif
