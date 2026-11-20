#include "ignore.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdio.h>

typedef struct {
    ah_paths *paths;
    const ah_tool_call *call;
    ah_hash *topology;
    const ah_path_filter *filter;
    size_t ignore_remaining;
    bool full;
} walk;

static int compare_paths(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}
static bool reserved(const char *name) {
    return !strcmp(name, ".git") || !strncmp(name, ".ah-write-", 10);
}
static ah_code add_file(walk *w, const char *path) {
    ah_paths *p = w->paths;
    if (w->filter && fnmatch(w->filter->glob, path, 0))
        return AH_OK;
    size_t limit = w->filter && w->filter->files_only ? 16384 : 2048;
    if (p->count == limit) {
        p->truncated = w->full = true;
        return AH_OK;
    }
    if (p->count == p->capacity) {
        size_t cap = p->capacity ? p->capacity * 2 : 32;
        char **items = realloc(p->items, cap * sizeof(*items));
        if (!items)
            return AH_IO;
        p->items = items;
        p->capacity = cap;
    }
    char *copy = ah_strdup(path);
    if (!copy)
        return AH_IO;
    p->items[p->count++] = copy;
    return AH_OK;
}

static ah_code directory(walk *, int, const char *, unsigned, const ignore_file *);

static char entry_kind(const struct stat *st) {
    return S_ISDIR(st->st_mode) ? 'd' : S_ISREG(st->st_mode) ? 'f' : 'x';
}
static ah_code collect(walk *w, DIR *dir, const char *prefix, unsigned depth,
                       const ignore_file *rules) {
    ah_paths *paths = w->paths;
    while (!w->full) {
        ah_code code = ah_local_stopped(w->call);
        if (code != AH_OK)
            return code;
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry)
            return errno ? AH_IO : AH_OK;
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..") || reserved(name))
            continue;
        if (++paths->visited > 16384) {
            paths->truncated = w->full = true;
            break;
        }
        size_t complete, n = strlen(name);
        char path[4097];
        if (!ah_utf8_prefix(name, n, &complete) || complete != n ||
            snprintf(path, sizeof path, "%s%s%s", prefix, *prefix ? "/" : "", name) >=
                (int)sizeof path) {
            paths->skipped++;
            continue;
        }
        char kind;
#if defined(_DIRENT_HAVE_D_TYPE) || defined(__APPLE__)
        if (!w->topology && entry->d_type != DT_UNKNOWN)
            kind = entry->d_type == DT_DIR ? 'd' : entry->d_type == DT_REG ? 'f' : 'x';
        else
#endif
        {
            struct stat st;
            if (fstatat(dirfd(dir), name, &st, AT_SYMLINK_NOFOLLOW))
                return AH_IO;
            kind = entry_kind(&st);
            if (w->topology) {
                ah_hash_update(w->topology, path, strlen(path) + 1);
                ah_hash_update(w->topology, &kind, 1);
                if (kind == 'd')
                    ah_local_stat_hash(w->topology, &st);
            }
        }
        if (kind == 'x') {
            paths->skipped++;
            continue;
        }
        if (rules && find_ignored(rules, path, name, kind == 'd'))
            continue;
        if (kind == 'd') {
            int child = openat(dirfd(dir), name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (child < 0)
                return AH_IO;
            code = directory(w, child, path, depth + 1, rules);
        } else
            code = add_file(w, path);
        if (code != AH_OK)
            return code;
    }
    return AH_OK;
}
static ah_code directory(walk *w, int fd, const char *prefix, unsigned depth,
                         const ignore_file *parent) {
    if (depth > 32) {
        w->paths->truncated = true;
        close(fd);
        return AH_OK;
    }
    DIR *dir = fdopendir(fd);
    if (!dir) {
        close(fd);
        return AH_IO;
    }
    ignore_file here = {0};
    const ignore_file *rules = NULL;
    ah_code code = ah_local_stopped(w->call);
    if (code == AH_OK && w->filter && !w->filter->include_ignored) {
        code = find_ignore_load(&here, parent, fd, strlen(prefix) + (*prefix != 0),
                                &w->ignore_remaining);
        rules = &here;
    }
    if (code == AH_OK)
        code = collect(w, dir, prefix, depth, rules);
    find_ignore_free(&here);
    closedir(dir);
    return code;
}
static ah_code list_paths(ah_local_tools *tools, yyjson_val *scope, const ah_tool_call *call,
                          ah_paths *paths, ah_hash *topology, const ah_path_filter *filter) {
    memset(paths, 0, sizeof(*paths));
    if (topology)
        ah_hash_init(topology);
    const char *path = scope ? yyjson_get_str(scope) : ".";
    if (!path || !*path || (scope && strlen(path) != yyjson_get_len(scope)) || strlen(path) > 4096)
        return AH_INVALID;
    int fd = openat(tools->workspace_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return AH_IO;
    walk w = {.paths = paths,
              .call = call,
              .topology = topology,
              .filter = filter,
              .ignore_remaining = 1024 * 1024};
    const ignore_file *parent = NULL;
    ignore_file ancestors[64];
    unsigned loaded = 0;
    ah_code code = AH_OK;
    if (!strcmp(path, ".")) {
        code = directory(&w, fd, "", 0, NULL);
        fd = -1;
    } else {
        char suffix[4097];
        strcpy(suffix, path);
        char *name = suffix;
        for (unsigned depth = 0; code == AH_OK; depth++) {
            char *slash = strchr(name, '/');
            if (slash)
                *slash = 0;
            if (depth == 64 || !*name || !strcmp(name, ".") || !strcmp(name, "..") ||
                (filter && reserved(name))) {
                code = AH_INVALID;
                break;
            }
            if (filter && !filter->include_ignored) {
                ignore_file *here = &ancestors[loaded++];
                code = find_ignore_load(here, parent, fd, (size_t)(name - suffix),
                                        &w.ignore_remaining);
                if (code != AH_OK)
                    break;
                parent = here;
            }
            struct stat st;
            if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW)) {
                code = errno == ENOENT ? AH_NOT_FOUND : AH_IO;
                break;
            }
            bool is_dir = S_ISDIR(st.st_mode);
            if (!is_dir && (!S_ISREG(st.st_mode) || slash)) {
                code = AH_INVALID;
                break;
            }
            /* Earlier separators have already been restored in suffix. */
            if (parent && find_ignored(parent, suffix, name, is_dir))
                break;
            if (!is_dir) {
                code = add_file(&w, path);
                break;
            }
            int child = openat(fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (child < 0) {
                code = AH_IO;
                break;
            }
            close(fd);
            fd = child;
            if (!slash) {
                code = directory(&w, fd, path, 0, parent);
                fd = -1;
                break;
            }
            *slash = '/';
            name = slash + 1;
        }
    }
    if (fd >= 0)
        close(fd);
    while (loaded)
        find_ignore_free(&ancestors[--loaded]);
    if (paths->count > 1)
        qsort(paths->items, paths->count, sizeof(*paths->items), compare_paths);
    return code;
}
ah_code ah_find_paths(ah_local_tools *tools, yyjson_val *scope, const ah_tool_call *call,
                      ah_paths *paths, const ah_path_filter *filter) {
    return list_paths(tools, scope, call, paths, NULL, filter);
}
ah_code ah_local_paths(ah_local_tools *tools, yyjson_val *scope, const ah_tool_call *call,
                       ah_paths *paths, ah_hash *topology) {
    return list_paths(tools, scope, call, paths, topology, NULL);
}
void ah_local_paths_free(ah_paths *paths) {
    for (size_t i = 0; i < paths->count; i++)
        free(paths->items[i]);
    free(paths->items);
}
