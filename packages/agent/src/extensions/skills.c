#include "catalog_private.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

/* Stop at the closing delimiter: discovery never reads the instruction body. */
static ah_code frontmatter(ext_catalog *c, const char *path, char **out, ah_error *error) {
    *out = NULL;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return errno == ENOENT ? AH_NOT_FOUND : ah_fail(error, AH_IO, "Cannot read skill metadata");
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        close(fd);
        return ah_fail(error, AH_INVALID, "Skill is not a regular file");
    }
    char *data = malloc(EXT_FRONTMATTER_MAX + 1);
    if (!data) {
        close(fd);
        return ah_fail(error, AH_IO, "Out of memory");
    }
    size_t size = 0, start = 0;
    bool first = true, finished = false;
    ah_code rc = AH_OK;
    while (size < EXT_FRONTMATTER_MAX) {
        if (!ext_remaining(c)) {
            rc = AH_LIMIT;
            break;
        }
        char ch;
        ssize_t got = read(fd, &ch, 1);
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0) {
            rc = AH_IO;
            break;
        }
        if (!got)
            break;
        ++c->bytes;
        if (!ch) {
            rc = AH_INVALID;
            break;
        }
        data[size++] = ch;
        if (ch != '\n')
            continue;
        size_t length = size - start - 1;
        if (length && data[start + length - 1] == '\r')
            --length;
        bool delimiter = length == 3 && !memcmp(data + start, "---", 3);
        if (first && !delimiter) {
            rc = AH_INVALID;
            break;
        }
        if (!first && delimiter) {
            data[start] = 0;
            finished = true;
            break;
        }
        first = false;
        start = size;
    }
    close(fd);
    if (rc == AH_OK && !finished)
        rc = AH_INVALID;
    if (rc != AH_OK) {
        free(data);
        return ah_fail(error, rc,
                       rc == AH_LIMIT ? "Skill metadata byte budget exceeded"
                                      : "Malformed or oversized skill frontmatter");
    }
    *out = data;
    return AH_OK;
}

static char *trim(char *text) {
    while (isspace((unsigned char)*text))
        ++text;
    size_t n = strlen(text);
    while (n && isspace((unsigned char)text[n - 1]))
        text[--n] = 0;
    return text;
}

static char *scalar(const char *text) {
    size_t n = strlen(text);
    if (n >= 2 && text[0] == '"' && text[n - 1] == '"') {
        yyjson_doc *doc = yyjson_read(text, n, 0);
        const char *value = doc ? yyjson_get_str(yyjson_doc_get_root(doc)) : NULL;
        char *out = value ? ah_strdup(value) : NULL;
        yyjson_doc_free(doc);
        return out;
    }
    if (n >= 2 && text[0] == '\'' && text[n - 1] == '\'') {
        char *out = malloc(n), *p = out;
        if (!out)
            return NULL;
        for (size_t i = 1; i + 1 < n; ++i) {
            *p++ = text[i];
            if (text[i] == '\'' && text[i + 1] == '\'')
                ++i;
        }
        *p = 0;
        return out;
    }
    if (*text == '"' || *text == '\'')
        return NULL;
    char *out = ah_strdup(text);
    if (out) {
        char *comment = strstr(out, " #");
        if (comment)
            *comment = 0;
    }
    return out;
}

static char *metadata_value(const char *front, const char *key) {
    char *copy = ah_strdup(front), *state = NULL, *result = NULL;
    if (!copy)
        return NULL;
    size_t key_len = strlen(key);
    bool block = false;
    for (char *line = strtok_r(copy, "\n", &state); line; line = strtok_r(NULL, "\n", &state)) {
        bool indented = *line == ' ' || *line == '\t';
        if (block) {
            if (!indented)
                break;
            char *part = trim(line);
            size_t old = result ? strlen(result) : 0, added = strlen(part);
            char *next = realloc(result, old + added + 2);
            if (!next) {
                free(result);
                result = NULL;
                break;
            }
            result = next;
            if (old)
                result[old++] = ' ';
            memcpy(result + old, part, added + 1);
        } else if (!indented && !strncmp(line, key, key_len) && line[key_len] == ':') {
            char *value = trim(line + key_len + 1);
            if (*value == '|' || *value == '>') {
                block = true;
                result = ah_strdup("");
            } else {
                result = scalar(value);
                break;
            }
        }
    }
    free(copy);
    return result;
}

static ah_code add_skill(ext_catalog *c, const char *path, const char *plugin, const char *boundary,
                         ah_error *error) {
    char *canonical = realpath(path, NULL);
    if (!canonical)
        return errno == ENOENT ? AH_NOT_FOUND : ah_fail(error, AH_IO, "Cannot resolve skill path");
    if (boundary && !ext_inside(boundary, canonical)) {
        free(canonical);
        return ah_fail(error, AH_INVALID, "Plugin skill path escapes plugin root");
    }
    char *id = ext_id("skill", canonical);
    if (!id) {
        free(canonical);
        return ah_fail(error, AH_IO, "Out of memory");
    }
    if (ext_find(c, id)) {
        free(id);
        free(canonical);
        return AH_OK;
    }
    char *front = NULL;
    ah_code rc = frontmatter(c, canonical, &front, error);
    char *name = rc == AH_OK ? metadata_value(front, "name") : NULL;
    char *description = rc == AH_OK ? metadata_value(front, "description") : NULL;
    free(front);
    if (rc == AH_OK && (!name || !*name || !description || !*description))
        rc = ah_fail(error, AH_INVALID, "Skill frontmatter requires name and description");
    if (rc == AH_OK) {
        bool enabled = !ext_disabled(c, "skill", name, canonical);
        ext_entry entry = {.id = id,
                           .kind = "skill",
                           .name = name,
                           .description = description,
                           .path = canonical,
                           .plugin = (char *)plugin,
                           .enabled = enabled,
                           .available = true,
                           .reason = enabled ? "" : "disabled by configuration"};
        rc = ext_add(c, &entry, error);
    }
    free(name);
    free(description);
    free(id);
    free(canonical);
    return rc;
}

ah_code ext_skills_walk(ext_catalog *c, const char *path, const char *plugin, const char *boundary,
                        unsigned depth, ah_error *error) {
    struct stat st;
    if (stat(path, &st)) {
        if (errno == ENOENT || errno == ENOTDIR)
            return AH_OK;
        return ah_fail(error, AH_IO, "Cannot inspect skill directory");
    }
    if (S_ISREG(st.st_mode))
        return add_skill(c, path, plugin, boundary, error);
    if (!S_ISDIR(st.st_mode))
        return AH_OK;
    if (depth > EXT_DEPTH_MAX)
        return ah_fail(error, AH_LIMIT, "Skill directory depth exceeded");
    char *canonical = realpath(path, NULL);
    if (!canonical)
        return ah_fail(error, AH_IO, "Cannot resolve skill directory");
    if (boundary && !ext_inside(boundary, canonical)) {
        free(canonical);
        return ah_fail(error, AH_INVALID, "Plugin skill path escapes plugin root");
    }
    for (size_t i = 0; i < c->visited_count; ++i)
        if (!strcmp(c->visited[i], canonical)) {
            free(canonical);
            return AH_OK;
        }
    size_t retained = strlen(canonical) + 1 + sizeof(char *);
    if (retained > ext_remaining(c)) {
        free(canonical);
        return ah_fail(error, AH_LIMIT, "Skill directory metadata budget exceeded");
    }
    char **visited = realloc(c->visited, (c->visited_count + 1) * sizeof(*visited));
    if (!visited) {
        free(canonical);
        return ah_fail(error, AH_IO, "Out of memory");
    }
    c->visited = visited;
    c->visited[c->visited_count++] = canonical;
    c->retained += retained;
    char *skill = ext_join(canonical, "SKILL.md");
    if (!skill)
        return ah_fail(error, AH_IO, "Out of memory");
    ah_code rc = add_skill(c, skill, plugin, boundary, error);
    free(skill);
    if (rc != AH_NOT_FOUND)
        return rc;
    char **names = NULL;
    size_t count = 0;
    rc = ext_children(c, canonical, &names, &count, error);
    for (size_t i = 0; i < count && rc == AH_OK; ++i) {
        if (names[i][0] == '.' && strcmp(names[i], ".system"))
            continue;
        char *child = ext_join(canonical, names[i]);
        if (!child) {
            rc = ah_fail(error, AH_IO, "Out of memory");
            break;
        }
        if (!stat(child, &st) && S_ISDIR(st.st_mode))
            rc = ext_skills_walk(c, child, plugin, boundary, depth + 1, error);
        free(child);
    }
    ext_names_free(names, count);
    return rc;
}

static ah_code skill_root(ext_catalog *c, const char *base, const char *suffix, ah_error *error) {
    char *path = ext_join(base, suffix);
    ah_code rc = path ? ext_skills_walk(c, path, NULL, NULL, 0, error)
                      : ah_fail(error, AH_IO, "Out of memory");
    free(path);
    return rc;
}

ah_code ext_skills_load(ext_catalog *c, ah_error *error) {
    ah_code rc = skill_root(c, c->home, "skills", error);
    if (rc == AH_OK && *c->user_home)
        rc = skill_root(c, c->user_home, ".agents/skills", error);
    if (rc != AH_OK || !*c->workspace)
        return rc;
    char *dir = realpath(c->workspace, NULL);
    if (!dir)
        return ah_fail(error, AH_IO, "Cannot resolve explicit workspace");
    for (unsigned depth = 0; rc == AH_OK && depth < 128; ++depth) {
        rc = skill_root(c, dir, ".agents/skills", error);
        char *git = ext_join(dir, ".git");
        struct stat st;
        bool stop = git && !lstat(git, &st);
        free(git);
        if (stop || !strcmp(dir, "/"))
            break;
        char *slash = strrchr(dir, '/');
        if (!slash)
            break;
        if (slash == dir)
            slash[1] = 0;
        else
            *slash = 0;
    }
    free(dir);
    return rc;
}
