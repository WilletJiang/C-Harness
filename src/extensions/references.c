#include "extensions_internal.h"
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>

static bool same_stamp(const struct stat *a, const struct stat *b) {
#if defined(__APPLE__)
    return a->st_mtimespec.tv_sec == b->st_mtimespec.tv_sec &&
           a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
           a->st_ctimespec.tv_sec == b->st_ctimespec.tv_sec &&
           a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
#else
    return a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
           a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
#endif
}

static ah_code read_descriptor(int fd, size_t limit, char **text, size_t *length, ah_error *error) {
    *text = NULL;
    struct stat before, after;
    ah_code rc = AH_OK;
    if (fd < 0 || fstat(fd, &before) || !S_ISREG(before.st_mode))
        rc = ah_fail(error, AH_IO, "skill must resolve to a readable regular file");
    else if (before.st_size < 0 || (uint64_t)before.st_size > limit)
        rc = ah_fail(error, AH_LIMIT, "skill exceeds configured content budget");
    char *buffer = rc == AH_OK ? malloc((size_t)before.st_size + 1) : NULL;
    if (rc == AH_OK && !buffer)
        rc = ah_fail(error, AH_IO, "out of memory");
    size_t used = 0;
    while (rc == AH_OK && used < (size_t)before.st_size) {
        ssize_t n = read(fd, buffer + used, (size_t)before.st_size - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            rc = ah_fail(error, AH_IO, "skill changed or could not be read");
        else
            used += (size_t)n;
    }
    if (rc == AH_OK &&
        (fstat(fd, &after) || before.st_size != after.st_size || !same_stamp(&before, &after)))
        rc = ah_fail(error, AH_IO, "skill changed during read; retry explicitly");
    if (fd >= 0)
        close(fd);
    size_t complete = 0;
    if (rc == AH_OK &&
        (memchr(buffer, 0, used) || !ah_utf8_prefix(buffer, used, &complete) || complete != used))
        rc = ah_fail(error, AH_INVALID, "skill/reference must contain complete UTF-8 without NUL");
    if (rc != AH_OK) {
        free(buffer);
        return rc;
    }
    buffer[used] = 0;
    *text = buffer;
    *length = used;
    return AH_OK;
}
ah_code ext_read_text(const char *path, size_t limit, char **text, size_t *length,
                      ah_error *error) {
    return read_descriptor(open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW), limit, text,
                           length, error);
}
ah_code ext_read_text_at(const char *directory, const char *relative, size_t limit, char **text,
                         size_t *length, ah_error *error) {
    char *parts = ah_strdup(relative), *state = NULL;
    if (!parts)
        return ah_fail(error, AH_IO, "out of memory");
    int fd = open(directory, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    char *part = strtok_r(parts, "/", &state);
    while (fd >= 0 && part) {
        char *next = strtok_r(NULL, "/", &state);
        if (!strcmp(part, ".") || !strcmp(part, "..")) {
            close(fd);
            fd = -1;
            break;
        }
        int child = openat(
            fd, part, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW | (next ? O_DIRECTORY : 0));
        close(fd);
        fd = child;
        part = next;
    }
    free(parts);
    return read_descriptor(fd, limit, text, length, error);
}
ah_code ext_skill_read(ah_extensions *x, const ah_tool_call *call, yyjson_val *args,
                       ah_tool_result *result, ah_error *error) {
    const char *relative = ext_string(args, "path");
    if (!relative || !*relative || *relative == '/')
        return ah_fail(error, AH_INVALID, "skill reference needs a relative path");
    char *skill = NULL;
    ah_code rc = ext_catalog_skill(x->catalog, ext_string(args, "id"), &skill, error);
    if (rc != AH_OK)
        return rc;
    char *slash = strrchr(skill, '/');
    if (!slash) {
        free(skill);
        return ah_fail(error, AH_INVALID, "skill directory unavailable");
    }
    *slash = 0;
    char joined[PATH_MAX], resolved[PATH_MAX];
    int written = snprintf(joined, sizeof(joined), "%s/%s", skill, relative);
    size_t directory_bytes = strlen(skill);
    if (written < 0 || (size_t)written >= sizeof(joined) || !realpath(joined, resolved) ||
        strncmp(resolved, skill, directory_bytes) || resolved[directory_bytes] != '/')
        rc = ah_fail(error, AH_INVALID,
                     "reference must resolve inside the selected skill directory");
    char *text = NULL;
    size_t total = 0;
    if (rc == AH_OK)
        rc = ext_read_text_at(skill, resolved + directory_bytes + 1, x->config.max_result_bytes,
                              &text, &total, error);
    free(skill);
    size_t offset = ext_number(args, "offset", 0), length = ext_number(args, "limit", 4096);
    if (rc == AH_OK &&
        (offset > total || (offset < total && ((unsigned char)text[offset] & 0xc0) == 0x80)))
        rc = ah_fail(error, AH_INVALID, "reference offset must be a valid UTF-8 byte boundary");
    if (rc != AH_OK) {
        free(text);
        return rc;
    }
    if (length > total - offset)
        length = total - offset;
    size_t budget = call->model_output_limit;
    if (!budget || budget > call->output_limit)
        budget = call->output_limit;
    char revision[65];
    ah_hash_bytes(text, total, revision);
    for (;;) {
        while (length && ((unsigned char)text[offset + length] & 0xc0) == 0x80)
            --length;
        if (!length && offset < total) {
            rc = ah_fail(error, AH_LIMIT, "reference page cannot fit the next UTF-8 character");
            break;
        }
        yyjson_mut_doc *doc = ah_mut_doc_new();
        yyjson_mut_val *root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
        yyjson_mut_obj_add_strcpy(doc, root, "path", resolved);
        yyjson_mut_obj_add_strcpy(doc, root, "revision", revision);
        yyjson_mut_obj_add_uint(doc, root, "offset", offset);
        yyjson_mut_obj_add_uint(doc, root, "next_offset", offset + length);
        yyjson_mut_obj_add_uint(doc, root, "total_bytes", total);
        yyjson_mut_obj_add_bool(doc, root, "more", offset + length < total);
        yyjson_mut_obj_add_strncpy(doc, root, "content", text + offset, length);
        char *json = ah_mut_write(doc);
        ah_mut_doc_free(doc);
        if (!json) {
            rc = ah_fail(error, AH_IO, "cannot encode reference page");
            break;
        }
        if (budget >= 32 && strlen(json) <= budget - 32) {
            if (!ah_json_valid(json)) {
                free(json);
                rc = ah_fail(error, AH_INVALID, "reference must contain UTF-8 text");
            } else
                result->json = json;
            break;
        }
        free(json);
        if (!length) {
            rc = ah_fail(error, AH_LIMIT, "reference metadata exceeds output budget");
            break;
        }
        length /= 2;
    }
    free(text);
    return rc;
}
