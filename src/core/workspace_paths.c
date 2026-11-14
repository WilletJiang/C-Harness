#include "workspace_internal.h"
#include "../tools/local_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

static bool prefix(const char *grant, const char *path) {
    size_t length = strlen(grant);
    return !strcmp(grant, ".") || (!strcmp(grant, "/") && path[0] == '/') ||
           (!strncmp(grant, path, length) && (!path[length] || path[length] == '/'));
}

#ifdef __APPLE__
/* CF collation is a conservative collision filter, never an authority grant.
 * Query each parent: a nested mount can have different case semantics. */
static ah_code component_equal(int parent, const char *a, const char *b, bool *equal,
                               ah_error *error) {
    *equal = !strcmp(a, b);
    if (*equal)
        return AH_OK;
    long sensitive = fpathconf(parent, _PC_CASE_SENSITIVE);
    if (sensitive < 0)
        return ah_fail(error, AH_IO, "Cannot inspect workspace filename semantics");
    CFStringRef first = CFStringCreateWithCString(NULL, a, kCFStringEncodingUTF8);
    CFStringRef second = CFStringCreateWithCString(NULL, b, kCFStringEncodingUTF8);
    ah_code code = AH_OK;
    if (!first || !second)
        code = ah_fail(error, AH_INVALID, "Cannot compare workspace filename encoding");
    else {
        CFStringCompareFlags flags = kCFCompareNonliteral;
        if (!sensitive)
            flags |= kCFCompareCaseInsensitive;
        *equal = CFStringCompare(first, second, flags) == kCFCompareEqualTo;
    }
    if (first)
        CFRelease(first);
    if (second)
        CFRelease(second);
    return code;
}

static ah_code overlap(int root, const char *a, const char *b, bool *found, ah_error *error) {
    int parent = openat(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (parent < 0)
        return ah_fail(error, AH_IO, "Cannot inspect workspace path overlap");
    ah_code code = AH_OK;
    for (;;) {
        size_t na = strcspn(a, "/"), nb = strcspn(b, "/");
        char first[4097], second[4097];
        if (!na || !nb || na >= sizeof(first) || nb >= sizeof(second)) {
            code = ah_fail(error, AH_INVALID, "Invalid workspace path component");
            break;
        }
        memcpy(first, a, na);
        first[na] = 0;
        memcpy(second, b, nb);
        second[nb] = 0;
        bool equal = false;
        code = component_equal(parent, first, second, &equal, error);
        if (code != AH_OK || !equal)
            break;
        if (!a[na] || !b[nb]) {
            *found = true;
            break;
        }
        int next = openat(parent, first, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0) {
            code = ah_fail(error, AH_INVALID, "Workspace parent changed while comparing grants");
            break;
        }
        close(parent);
        parent = next;
        a += na + 1;
        b += nb + 1;
    }
    close(parent);
    return code;
}

/* F_GETPATH resolves stored spelling without trusting a collation approximation.
 * A not-yet-existing final component deliberately retains its exact grant spelling. */
static ah_code canonical(int root, const char *path, char result[PATH_MAX], ah_error *error) {
    if (!strcmp(path, "."))
        return fcntl(root, F_GETPATH, result) < 0
                   ? ah_fail(error, AH_IO, "Cannot resolve workspace path")
                   : AH_OK;
    ah_local_tools tools = {.workspace_fd = root};
    char leaf[4097];
    int parent = ah_local_parent_path(&tools, path, strlen(path), leaf);
    if (parent < 0)
        return ah_fail(error, AH_INVALID, "Workspace grant parent changed");
    int target = openat(parent, leaf, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    int failure = errno;
    ah_code code = AH_OK;
    if (target >= 0) {
        if (fcntl(target, F_GETPATH, result) < 0)
            code = ah_fail(error, AH_IO, "Cannot resolve workspace target");
        close(target);
    } else if (failure != ENOENT)
        code = ah_fail(error, AH_INVALID, "Workspace grant target changed");
    else if (fcntl(parent, F_GETPATH, result) < 0)
        code = ah_fail(error, AH_IO, "Cannot resolve workspace parent");
    else {
        size_t length = strlen(result), name_length = strlen(leaf);
        size_t separator = length && result[length - 1] != '/';
        if (length + separator + name_length + 1 > PATH_MAX)
            code = ah_fail(error, AH_LIMIT, "Canonical workspace path exceeds bound");
        else {
            if (separator)
                result[length] = '/';
            memcpy(result + length + separator, leaf, name_length + 1);
        }
    }
    close(parent);
    return code;
}
#endif

ah_code workspace_paths_overlap(int root, const char *a, const char *b, bool *found,
                                ah_error *error) {
    *found = prefix(a, b) || prefix(b, a);
#ifdef __APPLE__
    return *found ? AH_OK : overlap(root, a, b, found, error);
#else
    (void)root;
    (void)error;
    return AH_OK;
#endif
}

ah_code workspace_path_contains(int root, const char *grant, const char *path, bool *found,
                                ah_error *error) {
    *found = false;
#ifdef __APPLE__
    char canonical_grant[PATH_MAX], canonical_path[PATH_MAX];
    ah_code code = canonical(root, grant, canonical_grant, error);
    if (code == AH_OK)
        code = canonical(root, path, canonical_path, error);
    if (code == AH_OK)
        *found = prefix(canonical_grant, canonical_path);
    return code;
#else
    (void)root;
    (void)error;
    *found = prefix(grant, path);
    return AH_OK;
#endif
}
