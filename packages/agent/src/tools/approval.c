#include "local_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <strings.h>

void ah_local_tools_set_auto_approve(ah_local_tools *tools, bool enabled) {
    if (tools)
        tools->auto_approve = enabled;
}

bool ah_local_tools_auto_approve(const ah_local_tools *tools) {
    return tools && tools->auto_approve;
}

bool ah_local_auto_approve_target(const struct stat *st) {
    return S_ISREG(st->st_mode) && st->st_uid == getuid() && st->st_nlink == 1 &&
           !(st->st_mode & (0111 | S_ISUID | S_ISGID));
}

static bool ordinary_path(const char *path) {
    /* Hidden components include Git, CI configuration, and dotfiles. */
    for (const char *part = path; part; ) {
        if (!*part || *part == '.')
            return false;
        part = strchr(part, '/');
        if (part)
            ++part;
    }
    const char *leaf = strrchr(path, '/');
    leaf = leaf ? leaf + 1 : path;
    if (!strcasecmp(leaf, "credentials.json") || !strcasecmp(leaf, "auth.json"))
        return false;
    static const char *const protected[] = {
        ".db", ".db-journal", ".db-shm", ".db-wal", ".sqlite", ".sqlite3",
        ".sqlite-journal", ".sqlite-shm", ".sqlite-wal",
        ".sqlite3-journal", ".sqlite3-shm", ".sqlite3-wal", ".lock",
        ".pem", ".key", ".p12", ".pfx",
    };
    const char *suffix = strrchr(leaf, '.');
    if (suffix)
        for (size_t i = 0; i < sizeof(protected) / sizeof(*protected); i++)
            if (!strcasecmp(suffix, protected[i]))
                return false;
    return true;
}

static bool guarded_edit(ah_local_tools *tools, yyjson_val *entry, bool patch) {
    yyjson_val *path_value = yyjson_obj_get(entry, "path");
    const char *path = yyjson_get_str(path_value);
    if (!path || strlen(path) != yyjson_get_len(path_value) || !ordinary_path(path))
        return false;
    const char *revision = yyjson_get_str(yyjson_obj_get(entry, "base_revision"));
    bool create = !patch && yyjson_is_true(yyjson_obj_get(entry, "expect_absent"));
    if (create ? revision != NULL : !revision || strspn(revision, "0123456789abcdef") != 64)
        return false;
    char leaf[4097];
    int parent = ah_local_parent_fd(tools, path_value, leaf);
    if (parent < 0)
        return false;
    struct stat st;
    int found = fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW);
    int failure = errno;
    close(parent);
    if (create)
        return found < 0 && failure == ENOENT;
    if (found < 0 || !ah_local_auto_approve_target(&st) || st.st_size > AH_LOCAL_MAX_BYTES)
        return false;
    if (!patch)
        return yyjson_get_len(yyjson_obj_get(entry, "content")) > 0;

    /* A patch which empties an existing file needs a manual decision. */
    int64_t size = st.st_size;
    yyjson_val *edits = yyjson_obj_get(entry, "edits"), *edit;
    size_t i, count;
    yyjson_arr_foreach(edits, i, count, edit) {
        size += (int64_t)yyjson_get_len(yyjson_obj_get(edit, "new_text"));
        size -= (int64_t)yyjson_get_len(yyjson_obj_get(edit, "old_text"));
    }
    return size > 0;
}

bool ah_local_auto_approve_call(const ah_tool *tool, const char *arguments) {
    bool patch = tool->execute == ah_local_apply;
    if (!patch && tool->execute != ah_local_write_file)
        return false;
    ah_local_tools *tools = tool->context;
    if (!ah_local_tools_auto_approve(tools))
        return false;
    yyjson_doc *doc = NULL;
    if (ah_schema_parse(tool->parameters_json, arguments, &doc, NULL) != AH_OK)
        return false;
    yyjson_val *root = yyjson_doc_get_root(doc);
    bool allowed = !patch ? guarded_edit(tools, root, false)
                          : !yyjson_arr_size(yyjson_obj_get(root, "checks"));
    if (patch && allowed) {
        yyjson_val *files = yyjson_obj_get(root, "files"), *file;
        size_t i, count;
        yyjson_arr_foreach(files, i, count, file) {
            if (!guarded_edit(tools, file, true)) {
                allowed = false;
                break;
            }
        }
    }
    yyjson_doc_free(doc);
    return allowed;
}
