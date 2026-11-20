#ifndef AH_FIND_IGNORE_H
#define AH_FIND_IGNORE_H
#include "local_internal.h"

typedef struct {
    const char *pattern;
    bool include, anchored, directory;
} ignore_rule;

typedef struct ignore_file {
    const struct ignore_file *parent;
    size_t prefix, count, total;
    char *data;
    ignore_rule *rules;
} ignore_file;

ah_code find_ignore_load(ignore_file *, const ignore_file *, int, size_t, size_t *);
void find_ignore_free(ignore_file *);
bool find_ignored(const ignore_file *, const char *, const char *, bool);
#endif
