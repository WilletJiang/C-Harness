#include "catalog_private.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

char *ext_join(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    if (na > SIZE_MAX - nb - 2)
        return NULL;
    char *p = malloc(na + nb + 2);
    if (p)
        snprintf(p, na + nb + 2, "%s/%s", a, b);
    return p;
}

char *ext_id(const char *kind, const char *key) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *p = (const unsigned char *)key; *p; ++p) {
        hash ^= *p;
        hash *= UINT64_C(1099511628211);
    }
    char value[80];
    snprintf(value, sizeof(value), "%s:%016llx", kind, (unsigned long long)hash);
    return ah_strdup(value);
}

bool ext_inside(const char *root, const char *path) {
    size_t n = strlen(root);
    return !strncmp(root, path, n) &&
           (path[n] == '/' || path[n] == '\0' || (n == 1 && root[0] == '/'));
}

ah_code ext_read(ext_catalog *c, const char *path, size_t maximum, char **out, ah_error *error) {
    *out = NULL;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT || errno == ENOTDIR)
            return AH_NOT_FOUND;
        return ah_fail(error, AH_IO, "Cannot read extension metadata: %s", path);
    }
    struct stat st;
    ah_code rc = AH_OK;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode))
        rc = ah_fail(error, AH_INVALID, "Extension metadata is not a regular file: %s", path);
    else if (st.st_size < 0 || (uintmax_t)st.st_size > maximum ||
             (uintmax_t)st.st_size > ext_remaining(c))
        rc = ah_fail(error, AH_LIMIT, "Extension metadata byte budget exceeded");
    size_t size = rc == AH_OK ? (size_t)st.st_size : 0;
    char *data = rc == AH_OK ? malloc(size + 1) : NULL;
    if (rc == AH_OK && !data)
        rc = ah_fail(error, AH_IO, "Out of memory");
    size_t n = 0;
    while (rc == AH_OK && n < size) {
        ssize_t got = read(fd, data + n, size - n);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0) {
            rc = ah_fail(error, AH_IO, "Extension metadata changed while reading");
            break;
        }
        n += (size_t)got;
    }
    close(fd);
    c->bytes += n;
    if (rc != AH_OK) {
        free(data);
        return rc;
    }
    data[n] = '\0';
    if (memchr(data, 0, n)) {
        free(data);
        return ah_fail(error, AH_INVALID, "NUL in extension metadata");
    }
    *out = data;
    return AH_OK;
}

ah_code ext_json_file(ext_catalog *c, const char *path, yyjson_doc **out, ah_error *error) {
    char *data = NULL;
    *out = NULL;
    ah_code rc = ext_read(c, path, EXT_METADATA_MAX, &data, error);
    if (rc != AH_OK)
        return rc;
    *out = yyjson_read(data, strlen(data), 0);
    free(data);
    if (!*out || !yyjson_is_obj(yyjson_doc_get_root(*out)) ||
        ah_json_check(yyjson_doc_get_root(*out), NULL) != AH_OK) {
        yyjson_doc_free(*out);
        *out = NULL;
        return ah_fail(error, AH_INVALID, "Malformed extension JSON: %s", path);
    }
    return AH_OK;
}

static int compare_names(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

void ext_names_free(char **names, size_t count) {
    for (size_t i = 0; i < count; ++i)
        free(names[i]);
    free(names);
}

ah_code ext_children(ext_catalog *c, const char *path, char ***out, size_t *count,
                     ah_error *error) {
    *out = NULL;
    *count = 0;
    DIR *dir = opendir(path);
    if (!dir) {
        if (errno == ENOENT || errno == ENOTDIR)
            return AH_OK;
        return ah_fail(error, AH_IO, "Cannot list extension directory: %s", path);
    }
    struct dirent *item;
    ah_code rc = AH_OK;
    while ((item = readdir(dir))) {
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, ".."))
            continue;
        if (++c->steps > c->options.max_catalog_entries * 32) {
            rc = ah_fail(error, AH_LIMIT, "Extension directory budget exceeded");
            break;
        }
        size_t bytes = strlen(item->d_name) + 1 + sizeof(char *);
        if (bytes > ext_remaining(c)) {
            rc = ah_fail(error, AH_LIMIT, "Extension directory metadata byte budget exceeded");
            break;
        }
        c->bytes += bytes;
        char **next = realloc(*out, (*count + 1) * sizeof(**out));
        if (!next) {
            rc = ah_fail(error, AH_IO, "Out of memory");
            break;
        }
        *out = next;
        next[*count] = ah_strdup(item->d_name);
        if (!next[*count]) {
            rc = ah_fail(error, AH_IO, "Out of memory");
            break;
        }
        ++*count;
    }
    closedir(dir);
    if (rc == AH_OK && *count)
        qsort(*out, *count, sizeof(**out), compare_names);
    if (rc != AH_OK) {
        ext_names_free(*out, *count);
        *out = NULL;
        *count = 0;
    }
    return rc;
}

ext_entry *ext_find(ext_catalog *c, const char *id) {
    for (size_t i = 0; i < c->count; ++i)
        if (!strcmp(c->entries[i].id, id))
            return &c->entries[i];
    return NULL;
}

static void entry_free(ext_entry *e) {
    free(e->id);
    free(e->kind);
    free(e->name);
    free(e->description);
    free(e->path);
    free(e->plugin);
    free(e->reason);
    free(e->config);
    memset(e, 0, sizeof(*e));
}

ah_code ext_add(ext_catalog *c, const ext_entry *entry, ah_error *error) {
    ext_entry *existing = ext_find(c, entry->id);
    if (existing) {
        if (!strcmp(existing->path, entry->path))
            return AH_OK;
        return ah_fail(error, AH_INVALID, "Extension ID collision");
    }
    if (c->count >= c->options.max_catalog_entries)
        return ah_fail(error, AH_LIMIT, "Extension entry budget exceeded");
    size_t retained = 0;
    const char *strings[] = {entry->id,   entry->kind,   entry->name,   entry->description,
                             entry->path, entry->plugin, entry->reason, entry->config};
    for (size_t i = 0; i < sizeof(strings) / sizeof(*strings); ++i)
        retained += strings[i] ? strlen(strings[i]) + 1 : 1;
    if (retained > ext_remaining(c))
        return ah_fail(error, AH_LIMIT, "Extension retained metadata budget exceeded");
    ext_entry e = *entry;
#define COPY(field) e.field = ah_strdup(entry->field ? entry->field : "")
    COPY(id);
    COPY(kind);
    COPY(name);
    COPY(description);
    COPY(path);
    COPY(plugin);
    COPY(reason);
    COPY(config);
#undef COPY
    if (!e.id || !e.kind || !e.name || !e.description || !e.path || !e.plugin || !e.reason ||
        !e.config) {
        entry_free(&e);
        return ah_fail(error, AH_IO, "Out of memory");
    }
    c->entries[c->count++] = e;
    c->retained += retained;
    return AH_OK;
}

void ext_catalog_close(ext_catalog *c) {
    if (!c)
        return;
    for (size_t i = 0; i < c->count; ++i)
        entry_free(&c->entries[i]);
    for (size_t i = 0; i < 2; ++i)
        toml_free(c->config[i]);
    ext_names_free(c->visited, c->visited_count);
    free(c->entries);
    free(c->home);
    free(c->workspace);
    free(c->user_home);
    pthread_mutex_destroy(&c->mutex);
    free(c);
}
