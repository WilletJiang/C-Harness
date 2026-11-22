#include "ignore.h"
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>

/* One bounded stack of workspace-local .gitignore files. A symlink is never
 * followed, including when it is itself named .gitignore. */
ah_code find_ignore_load(ignore_file *file, const ignore_file *parent, int dir, size_t prefix,
                         size_t *remaining) {
    *file = (ignore_file){.parent = parent, .prefix = prefix, .total = parent ? parent->total : 0};
    struct stat named, st, after;
    if (fstatat(dir, ".gitignore", &named, AT_SYMLINK_NOFOLLOW))
        return errno == ENOENT ? AH_OK : AH_IO;
    if (!S_ISREG(named.st_mode))
        return AH_OK;
    int fd = openat(dir, ".gitignore", O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0)
        return AH_IO;
    ah_code code = AH_OK;
    if (fstat(fd, &st) || !ah_local_same_file(&named, &st))
        code = AH_IO;
    else if (st.st_size < 0 || st.st_size > 65536 || (size_t)st.st_size > *remaining)
        code = AH_LIMIT;
    else
        *remaining -= (size_t)st.st_size;
    if (code == AH_OK && !(file->data = malloc((size_t)st.st_size + 1)))
        code = AH_IO;
    size_t size = 0;
    while (code == AH_OK && size < (size_t)st.st_size) {
        ssize_t n = read(fd, file->data + size, (size_t)st.st_size - size);
        if (n > 0)
            size += (size_t)n;
        else if (n < 0 && errno == EINTR)
            continue;
        else
            code = AH_IO;
    }
    if (code == AH_OK && (fstat(fd, &after) || !ah_local_same_file(&st, &after)))
        code = AH_IO;
    close(fd);
    if (code != AH_OK)
        return code;
    if (memchr(file->data, 0, size))
        return AH_INVALID;
    file->data[size] = 0;
    size_t cap = 0;
    char *line = file->data;
    if (size >= 3 && !memcmp(line, "\xef\xbb\xbf", 3))
        line += 3;
    while (line) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        size_t n = strlen(line);
        if (n && line[n - 1] == '\r')
            line[--n] = 0;
        while (n && line[n - 1] == ' ') {
            size_t i = n - 1;
            while (i && line[i - 1] == '\\')
                --i;
            if ((n - 1 - i) % 2)
                break;
            line[--n] = 0;
        }
        if (n && line[0] != '#') {
            bool include = line[0] == '!';
            if (include) {
                ++line;
                --n;
            }
            bool directory = n && line[n - 1] == '/';
            if (directory)
                line[--n] = 0;
            bool anchored = strchr(line, '/') != NULL;
            if (*line == '/') {
                ++line;
                --n;
            }
            if (n) {
                if (file->total == 1024 || n > 4096)
                    return AH_LIMIT;
                if (file->count == cap) {
                    cap = cap ? cap * 2 : 8;
                    ignore_rule *rules = realloc(file->rules, cap * sizeof(*rules));
                    if (!rules)
                        return AH_IO;
                    file->rules = rules;
                }
                file->rules[file->count++] = (ignore_rule){line, include, anchored, directory};
                ++file->total;
            }
        }
        line = next;
    }
    return AH_OK;
}

void find_ignore_free(ignore_file *file) {
    free(file->data);
    free(file->rules);
}

/* Slash-delimited ** spans whole components. Dynamic programming bounds the
 * work even with repeated globstars. Normal patterns use POSIX fnmatch. */
static bool path_match(const char *pattern, const char *path) {
    if (!strstr(pattern, "**"))
        return fnmatch(pattern, path, FNM_PATHNAME) == 0;
    char components[4097], patterns[4097];
    if (strlen(path) >= sizeof components || strlen(pattern) >= sizeof patterns)
        return false;
    strcpy(components, path);
    strcpy(patterns, pattern);
    char *parts[96], *save = NULL;
    size_t count = 0;
    for (char *part = strtok_r(components, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (count == sizeof(parts) / sizeof(*parts))
            return false;
        parts[count++] = part;
    }
    bool matches[97] = {true};
    for (char *pat = patterns; pat;) {
        char *next = strchr(pat, '/');
        if (next)
            *next++ = 0;
        if (!strcmp(pat, "**")) {
            if (!next) { /* trailing slash-star-star covers descendants only */
                for (size_t j = 0; j < count; j++)
                    if (matches[j])
                        return true;
                return false;
            }
            for (size_t j = 1; j <= count; j++)
                matches[j] |= matches[j - 1];
        } else {
            for (size_t j = count; j; j--)
                matches[j] = matches[j - 1] && !fnmatch(pat, parts[j - 1], 0);
            matches[0] = false;
        }
        pat = next;
    }
    return matches[count];
}

bool find_ignored(const ignore_file *file, const char *path, const char *name, bool directory) {
    for (; file; file = file->parent) {
        for (size_t i = file->count; i; i--) {
            const ignore_rule *r = &file->rules[i - 1];
            if (r->directory && !directory)
                continue;
            if (r->anchored ? path_match(r->pattern, path + file->prefix)
                            : !fnmatch(r->pattern, name, 0))
                return !r->include;
        }
    }
    return false;
}
