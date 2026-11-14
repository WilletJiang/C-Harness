#include "core/workspace_internal.h"
#include "tools/local_internal.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>

static void okay(ah_code code, const ah_error *error) {
    if (code != AH_OK) {
        fprintf(stderr, "%s: %s\n", ah_code_name(code), error->message);
        abort();
    }
}
static void release(ah_engine *e, const char *a, const char *b) {
    ah_error error = {0};
    okay(ah_workspace_assign(e, a, NULL, 0, false, &error), &error);
    okay(ah_workspace_assign(e, b, NULL, 0, false, &error), &error);
}
static void put(int root, const char *name, const char *text) {
    int fd = openat(root, name, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    assert(fd >= 0 && write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    close(fd);
}
static bool alias(int root, const char *a, const char *b) {
    struct stat first, second;
    assert(fstatat(root, a, &first, AT_SYMLINK_NOFOLLOW) == 0);
    return fstatat(root, b, &second, AT_SYMLINK_NOFOLLOW) == 0 && first.st_dev == second.st_dev &&
           first.st_ino == second.st_ino;
}
static ah_code authorized(ah_engine *e, ah_local_tools *tools, const char *sid, const char *path) {
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "path", path);
    char *arguments = ah_mut_write(doc);
    assert(arguments);
    core_operation op = {.effect = AH_EFFECT_WRITE, .arguments = arguments};
    core_tool tool = {
        .tool = {.execute = ah_local_write_file, .effect = AH_EFFECT_WRITE, .context = tools}};
    bool coordinated = false;
    ah_error error = {0};
    ah_code code = core_workspace_authorize(e, sid, &op, &tool, &coordinated, &error);
    assert(coordinated);
    free(arguments);
    ah_mut_doc_free(doc);
    return code;
}
static void pair(ah_engine *e, const char *a, const char *b, const char *first, const char *second,
                 bool conflicts) {
    release(e, a, b);
    ah_error error = {0};
    okay(ah_workspace_assign(e, a, &first, 1, false, &error), &error);
    assert(ah_workspace_assign(e, b, &second, 1, false, &error) == (conflicts ? AH_BUSY : AH_OK));
    release(e, a, b);
    const char *both[] = {first, second};
    assert(ah_workspace_assign(e, a, both, 2, false, &error) == (conflicts ? AH_INVALID : AH_OK));
}
static void directory_case(ah_engine *e, ah_local_tools *tools, const char *a, const char *b,
                           const char *first, const char *second) {
    int root = tools->workspace_fd;
    assert(mkdirat(root, first, 0700) == 0);
    bool same = alias(root, first, second);
    if (!same)
        assert(mkdirat(root, second, 0700) == 0);
    char inside[128];
    snprintf(inside, sizeof(inside), "%s/new.txt", second);
    pair(e, a, b, first, inside, same);
    release(e, a, b);
    ah_error error = {0};
    okay(ah_workspace_assign(e, a, &first, 1, false, &error), &error);
    assert(authorized(e, tools, a, inside) == (same ? AH_OK : AH_AUTH));
    release(e, a, b);
    assert(unlinkat(root, first, AT_REMOVEDIR) == 0);
    if (!same)
        assert(unlinkat(root, second, AT_REMOVEDIR) == 0);
}
static void hardlinks(ah_engine *e, ah_local_tools *tools, const char *a, const char *b) {
    int root = tools->workspace_fd;
    put(root, "hard-a.txt", "original");
    assert(linkat(root, "hard-a.txt", root, "hard-b.txt", 0) == 0);
    assert(alias(root, "hard-a.txt", "hard-b.txt"));
    pair(e, a, b, "hard-a.txt", "hard-b.txt", false);
    release(e, a, b);
    const char *path = "hard-a.txt";
    ah_error error = {0};
    okay(ah_workspace_assign(e, a, &path, 1, false, &error), &error);
    assert(authorized(e, tools, a, "hard-b.txt") == AH_AUTH);
    char digest[65], arguments[256];
    ah_hash_bytes("original", 8, digest);
    snprintf(arguments, sizeof(arguments),
             "{\"path\":\"hard-a.txt\",\"content\":\"replacement\",\"base_revision\":\"%s\"}",
             digest);
    ah_tool_call call = {.session_id = a,
                         .arguments_json = arguments,
                         .output_limit = 4096,
                         .deadline_ms = ah_now_ms() + 1000,
                         .require_revision = true};
    ah_tool_result result = {0};
    okay(ah_local_write_file(tools, &call, &result, &error), &error);
    assert(!result.is_error && !result.outcome_unknown);
    free(result.json);
    char text[16] = {0};
    int fd = openat(root, "hard-b.txt", O_RDONLY | O_CLOEXEC);
    assert(fd >= 0 && read(fd, text, sizeof(text)) == 8 && !strcmp(text, "original"));
    close(fd);
    assert(!alias(root, "hard-a.txt", "hard-b.txt"));
    release(e, a, b);
    assert(unlinkat(root, "hard-a.txt", 0) == 0 && unlinkat(root, "hard-b.txt", 0) == 0);
}
void workspace_alias_cases(const ah_config *base) {
    char directory[] = "/tmp/ah-workspace-alias-XXXXXX";
    assert(mkdtemp(directory));
    char database[256], lock[272], a[AH_ID_CAP], b[AH_ID_CAP];
    snprintf(database, sizeof(database), "%s/history.db", directory);
    snprintf(lock, sizeof(lock), "%s.lock", database);
    ah_config config = *base;
    config.database_path = database;
    ah_engine *e = NULL;
    ah_error error = {0};
    okay(ah_open(&config, &e, &error), &error);
    okay(ah_session_create(e, "fixture", "", a, &error), &error);
    okay(ah_session_create(e, "fixture", "", b, &error), &error);
    okay(ah_workspace_enable(e, directory, &error), &error);
    ah_local_tools *tools = NULL;
    ah_local_tools_config local = {.workspace = directory};
    okay(ah_local_tools_open(&local, &tools, &error), &error);
    int root = tools->workspace_fd;
    put(root, "Case.txt", "data");
    bool insensitive = alias(root, "Case.txt", "case.txt");
    pair(e, a, b, "Case.txt", "case.txt", insensitive);
    pair(e, a, b, "Future.txt", "future.txt", insensitive);
    pair(e, a, b, "ÄFuture.txt", "äfuture.txt", insensitive);
    release(e, a, b);
    const char *future = "Future.txt", *existing = "Case.txt";
    okay(ah_workspace_assign(e, a, &future, 1, false, &error), &error);
    assert(authorized(e, tools, a, "Future.txt") == AH_OK);
    assert(authorized(e, tools, a, "future.txt") == AH_AUTH);
    okay(ah_workspace_assign(e, a, &existing, 1, false, &error), &error);
    assert(authorized(e, tools, a, "case.txt") == (insensitive ? AH_OK : AH_AUTH));
    ah_close(e);
    okay(ah_open(&config, &e, &error), &error);
    const char *other = "case.txt";
    assert(ah_workspace_assign(e, b, &other, 1, false, &error) == (insensitive ? AH_BUSY : AH_OK));
    directory_case(e, tools, a, b, "Directory", "directory");
    directory_case(e, tools, a, b, "Ärea", "ärea");
    directory_case(e, tools, a, b, "Café", "Cafe\xcc\x81");
    hardlinks(e, tools, a, b);
    release(e, a, b);
    okay(ah_workspace_assign(e, a, &existing, 1, false, &error), &error);
    sqlite3_stmt *s = NULL;
    okay(core_prepare(e,
                      "INSERT INTO agent_nodes(session_id,root_id,parent_id,name,depth) "
                      "VALUES(?1,?1,NULL,'root',0),(?2,?1,?1,'child',1)",
                      &s, &error),
         &error);
    sqlite3_bind_text(s, 1, a, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, b, -1, SQLITE_TRANSIENT);
    okay(core_done(e, s, &error), &error);
    assert(ah_workspace_transfer(e, a, b, "case.txt", &error) == AH_AUTH);
    okay(ah_workspace_transfer(e, a, b, "Case.txt", &error), &error);
    ah_close(e);
    assert(unlinkat(root, "Case.txt", 0) == 0);
    ah_local_tools_close(tools);
    assert(unlink(database) == 0 && unlink(lock) == 0 && rmdir(directory) == 0);
}
