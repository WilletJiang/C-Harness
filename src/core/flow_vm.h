#ifndef AH_FLOW_VM_H
#define AH_FLOW_VM_H
#include "core.h"
#include <sys/types.h>

/* The journal owns execution. This cache retains only a live JS continuation. */
typedef struct core_flow_cell {
    char operation[AH_ID_CAP];
    uint64_t next_generation;
    unsigned issued;
    sqlite3_int64 last_seq;
    size_t history_bytes;
    bool disconnected;
    int fd;
    pid_t pid;
    struct core_flow_cell *next;
} core_flow_cell;

core_flow_cell *core_flow_lookup(ah_engine *, const ah_tool_call *);
ah_code core_flow_start(ah_engine *, const char *, core_flow_cell **, ah_error *);
void core_flow_drop(ah_engine *, core_flow_cell *);
ah_code core_flow_exchange(core_flow_cell *, const ah_tool_call *, const char *, char **,
                           ah_error *);
#endif
