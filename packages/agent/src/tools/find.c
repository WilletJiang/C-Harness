#include "local_internal.h"
#include <regex.h>

const char ah_find_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"mode\":{\"type\":\"string\",\"enum\":[\"files\",\"content\"]},"
    "\"include_ignored\":{\"type\":\"boolean\"},"
    "\"force\":{\"type\":\"boolean\"},"
    "\"path\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":4096},"
    "\"query\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":1024},"
    "\"syntax\":{\"type\":\"string\",\"enum\":[\"literal\",\"ere\"]},"
    "\"glob\":{\"type\":\"string\",\"maxLength\":256},"
    "\"context_lines\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":5},"
    "\"max_results\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100},"
    "\"offset\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":10000000},"
    "\"snapshot\":{\"type\":\"string\",\"minLength\":64,\"maxLength\":64}},"
    "\"additionalProperties\":false}";

typedef struct {
    yyjson_mut_doc *doc;
    yyjson_mut_val *results;
    size_t offset, total, emitted, max_results, remaining;
    unsigned context;
    bool full;
} find_output;

enum { FIND_ALTERNATIVES = 16, FIND_QUERY_BYTES = 1024 };

typedef struct {
    regex_t regex;
    const char *alternatives[FIND_ALTERNATIVES];
    size_t count;
    bool compiled;
} find_pattern;

static size_t literal_alternatives(const char *query, size_t length, char *storage,
                                   const char **alternatives) {
    if (length > FIND_QUERY_BYTES || strpbrk(query, ".[]\\*^$+?(){}"))
        return 0;
    memcpy(storage, query, length + 1);
    size_t count = 0;
    char *part = storage;
    for (char *bar = strchr(part, '|'); bar; bar = strchr(part, '|')) {
        if (bar == part || count == FIND_ALTERNATIVES - 1)
            return 0;
        *bar = '\0';
        alternatives[count++] = part;
        part = bar + 1;
    }
    if (!count || !*part)
        return 0;
    alternatives[count++] = part;
    return count;
}

/* Each alternative advances only when its cached hit has already been covered. */
static const char *alternative_match(const find_pattern *pattern, const char *start,
                                     const char *end, const char **hits) {
    const char *first = end;
    for (size_t i = 0; i < pattern->count; ++i) {
        if (hits[i] && hits[i] < start)
            hits[i] = strstr(start, pattern->alternatives[i]);
        if (hits[i] && hits[i] < first)
            first = hits[i];
    }
    return first < end ? first : NULL;
}

static void excerpt(find_output *out, const char *path, const char *revision, const char *source,
                    size_t offset, size_t end, size_t first, size_t last, const char *query) {
    size_t ordinal = out->total++;
    if (ordinal < out->offset || out->full)
        return;
    size_t length = end - offset;
    yyjson_mut_val *row = yyjson_mut_obj(out->doc);
    yyjson_mut_obj_add_strcpy(out->doc, row, "path", path);
    yyjson_mut_obj_add_strcpy(out->doc, row, "revision", revision);
    yyjson_mut_obj_add_uint(out->doc, row, "start_line", first + 1);
    yyjson_mut_obj_add_uint(out->doc, row, "end_line", last + 1);
    yyjson_mut_obj_add_strcpy(out->doc, row, "match", query);
    yyjson_mut_obj_add_uint(out->doc, row, "offset_bytes", offset);
    /* Cap individual excerpts too; the continuation is an exact read_file offset. */
    size_t cap = out->remaining > 1024 ? (out->remaining - 1024) / 6 : 0;
    if (cap > 8192)
        cap = 8192;
    size_t keep = length < cap ? length : cap, complete = 0;
    if (!keep) {
        out->full = true;
        return;
    }
    if (!ah_utf8_prefix(source + offset, keep, &complete)) {
        out->full = true;
        return;
    }
    keep = complete;
    yyjson_mut_obj_add_strncpy(out->doc, row, "content", source + offset, keep);
    yyjson_mut_obj_add_bool(out->doc, row, "truncated", keep < length);
    if (keep < length)
        yyjson_mut_obj_add_uint(out->doc, row, "next_offset_bytes", offset + keep);
    char *encoded = yyjson_mut_val_write(row, 0, NULL);
    size_t bytes = encoded ? strlen(encoded) + 1 : SIZE_MAX;
    free(encoded);
    if (bytes > out->remaining) {
        out->full = true;
        return;
    }
    yyjson_mut_arr_append(out->results, row);
    out->remaining -= bytes;
    out->emitted++;
    if (out->emitted >= out->max_results)
        out->full = true;
}

static bool has_backreference(const char *pattern) {
    for (const char *p = pattern; *p; ++p) {
        if (*p == '\\' && p[1]) {
            ++p;
            if (*p >= '1' && *p <= '9')
                return true;
        }
    }
    return false;
}

/* Each invocation sees a single line, including for expressions such as [[:space:]].
 * Check cancellation on sparse searches too and avoid allocating line copies. */
static ah_code find_regex_line(const regex_t *regex, char *text, size_t length,
                               const ah_tool_call *call, const char **match,
                               const char **after_line) {
    *match = NULL;
    size_t lines = 0;
    char *end = text + length;
    for (char *line = text; line < end;) {
        if (!(lines++ % 256)) {
            ah_code code = ah_local_stopped(call);
            if (code != AH_OK)
                return code;
        }
        char *boundary = memchr(line, '\n', (size_t)(end - line));
#ifdef REG_STARTEND
        regmatch_t bounds = {.rm_so = 0, .rm_eo = (regoff_t)((boundary ? boundary : end) - line)};
        int status = regexec(regex, line, 1, &bounds, REG_STARTEND);
#else
        if (boundary)
            *boundary = '\0';
        int status = regexec(regex, line, 0, NULL, 0);
        if (boundary)
            *boundary = '\n';
#endif
        if (!status) {
            *match = line;
            *after_line = boundary ? boundary + 1 : end;
            return AH_OK;
        }
        if (status != REG_NOMATCH)
            return status == REG_ESPACE ? AH_LIMIT : AH_IO;
        line = boundary ? boundary + 1 : end;
    }
    return AH_OK;
}

/* A matching line contributes one context range, regardless of hit count.
 * Retain only the preceding context line starts, not an index for the whole file. */
static ah_code find_source(find_output *out, const char *path, const char *revision,
                           char *source, size_t length, const char *query, const find_pattern *pattern,
                           const ah_tool_call *call) {
    const char *match = NULL, *after_line = NULL;
    const char *hits[FIND_ALTERNATIVES];
    ah_code code = AH_OK;
    if (pattern->compiled)
        code = find_regex_line(&pattern->regex, source, length, call, &match, &after_line);
    else if (pattern->count) {
        for (size_t i = 0; i < pattern->count; ++i)
            hits[i] = strstr(source, pattern->alternatives[i]);
        match = alternative_match(pattern, source, source + length, hits);
    } else
        match = strstr(source, query);
    if (code != AH_OK)
        return code;
    size_t previous[6] = {0}, cursor = 0, line = 0;
    size_t group_start = 0, group_end = 0, first = 0, last = 0;
    bool pending = false;
    while (match) {
        code = ah_local_stopped(call);
        if (code != AH_OK)
            return code;
        size_t position = (size_t)(match - source);
        const char *newline;
        while ((newline = memchr(source + cursor, '\n', position - cursor))) {
            cursor = (size_t)(newline - source) + 1;
            previous[++line % 6] = cursor;
            if (!(line % 256) && (code = ah_local_stopped(call)) != AH_OK)
                return code;
        }
        size_t start_line = line > out->context ? line - out->context : 0;
        size_t start = previous[start_line % 6];
        size_t next_line;
        if (pattern->compiled)
            next_line = (size_t)(after_line - source);
        else {
            newline = memchr(match, '\n', length - position);
            next_line = newline ? (size_t)(newline - source) + 1 : length;
        }
        size_t end = next_line, end_line = line;
        for (unsigned i = 0; i < out->context && end < length; i++) {
            newline = memchr(source + end, '\n', length - end);
            end = newline ? (size_t)(newline - source) + 1 : length;
            end_line++;
        }
        if (pending && start_line <= last + 1) {
            if (end > group_end) {
                group_end = end;
                last = end_line;
            }
        } else {
            if (pending)
                excerpt(out, path, revision, source, group_start, group_end, first, last, query);
            group_start = start;
            group_end = end;
            first = start_line;
            last = end_line;
            pending = true;
        }
        if (next_line == length)
            break;
        if (pattern->compiled)
            code = find_regex_line(&pattern->regex, source + next_line, length - next_line, call,
                                   &match, &after_line);
        else if (pattern->count)
            match = alternative_match(pattern, source + next_line, source + length, hits);
        else
            match = strstr(source + next_line, query);
        if (code != AH_OK)
            return code;
    }
    if (pending)
        excerpt(out, path, revision, source, group_start, group_end, first, last, query);
    return ah_local_stopped(call);
}

static void list_file(find_output *out, const char *path) {
    size_t ordinal = out->total++;
    if (ordinal < out->offset || out->full)
        return;
    yyjson_mut_val *row = yyjson_mut_obj(out->doc);
    yyjson_mut_obj_add_strcpy(out->doc, row, "path", path);
    size_t size = 0;
    char *json = yyjson_mut_val_write(row, 0, &size);
    if (!json || size + 1 > out->remaining)
        out->full = true;
    else {
        out->remaining -= size + 1;
        yyjson_mut_arr_append(out->results, row);
        out->full = ++out->emitted == out->max_results;
    }
    free(json);
}
ah_code ah_local_find(void *context, const ah_tool_call *call, ah_tool_result *result,
                      ah_error *error) {
    ah_error fallback;
    if (!error)
        error = &fallback;
    yyjson_doc *args = ah_local_arguments(call, ah_find_schema, result, error);
    if (!args)
        return error->code;
    yyjson_val *root = yyjson_doc_get_root(args), *scope = yyjson_obj_get(root, "path"),
               *q = yyjson_obj_get(root, "query"), *g = yyjson_obj_get(root, "glob"),
               *snap = yyjson_obj_get(root, "snapshot"), *syntax = yyjson_obj_get(root, "syntax");
    const char *query = yyjson_get_str(q), *glob = g ? yyjson_get_str(g) : "*";
    size_t offset = (size_t)yyjson_get_num(yyjson_obj_get(root, "offset"));
    bool files = yyjson_equals_str(yyjson_obj_get(root, "mode"), "files");
    bool ignored = yyjson_is_true(yyjson_obj_get(root, "include_ignored"));
    bool ere = yyjson_equals_str(syntax, "ere");
    if ((files && (q || syntax || yyjson_obj_get(root, "context_lines"))) ||
        (!files && (!query || memchr(query, 0, yyjson_get_len(q)) || strchr(query, '\n'))) ||
        (g && memchr(glob, 0, yyjson_get_len(g))) || (offset && !snap)) {
        yyjson_doc_free(args);
        return ah_local_error(call, result, error, AH_INVALID,
                              "Content search needs a single-line query; files mode omits "
                              "query/syntax/context_lines. Continue with a snapshot");
    }
    find_pattern pattern = {0};
    char alternative_storage[FIND_QUERY_BYTES + 1];
    bool expression = ere && strpbrk(query, ".[]\\*^$+?(){}|");
    if (expression)
        pattern.count = literal_alternatives(query, yyjson_get_len(q), alternative_storage,
                                             pattern.alternatives);
    if (expression && !pattern.count) {
        char message[256];
        int status = has_backreference(query) ? REG_ESUBREG
                                              : regcomp(&pattern.regex, query, REG_EXTENDED | REG_NOSUB);
        if (status) {
            if (status == REG_ESUBREG)
                strcpy(message, "ERE backreferences are unsupported");
            else
                regerror(status, &pattern.regex, message, sizeof(message));
            yyjson_doc_free(args);
            return ah_local_error(call, result, error, status == REG_ESPACE ? AH_LIMIT : AH_INVALID,
                                  message);
        }
        pattern.compiled = true;
    }
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *output = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, output);
    find_output out = {.doc = doc,
                       .results = yyjson_mut_obj_add_arr(doc, output, "results"),
                       .offset = offset,
                       .context = 2,
                       .max_results = 20};
    yyjson_val *v = yyjson_obj_get(root, "context_lines");
    if (v)
        out.context = (unsigned)yyjson_get_num(v);
    v = yyjson_obj_get(root, "max_results");
    if (v)
        out.max_results = (size_t)yyjson_get_num(v);
    size_t limit = ah_local_view_limit(call);
    size_t reserve = files ? 384 : 1024;
    out.remaining = limit > reserve ? limit - reserve : 0;
    ah_paths paths;
    ah_path_filter filter = {.glob = glob, .files_only = files, .include_ignored = ignored};
    ah_code code = ah_find_paths(context, scope, call, &paths, &filter);
    ah_hash hash;
    ah_hash_init(&hash);
    unsigned char mode[] = {files, ignored};
    ah_hash_update(&hash, mode, sizeof mode);
    if (query)
        ah_hash_update(&hash, query, strlen(query) + 1);
    if (ere)
        ah_hash_update(&hash, "\0ere", 4);
    ah_hash_update(&hash, glob, strlen(glob) + 1);
    const char *scope_path = scope ? yyjson_get_str(scope) : ".";
    ah_hash_update(&hash, scope_path, strlen(scope_path) + 1);
    unsigned char context_lines = (unsigned char)out.context;
    ah_hash_update(&hash, &context_lines, 1);
    ah_scan_budget budget = {.remaining = 32 * 1024 * 1024};
    size_t scanned = 0, skipped = paths.skipped;
    for (size_t i = 0; code == AH_OK && i < paths.count; i++) {
        if (files) {
            code = ah_local_stopped(call);
            ah_hash_update(&hash, paths.items[i], strlen(paths.items[i]) + 1);
            list_file(&out, paths.items[i]);
            continue;
        }
        char leaf[4097], *source = NULL, revision[65];
        size_t length = 0;
        struct stat st;
        int parent = ah_local_parent_path(context, paths.items[i], strlen(paths.items[i]), leaf);
        code = parent < 0 ? AH_IO
                          : ah_local_source(parent, leaf, call, &st, &source, &length, &budget);
        if (parent >= 0)
            close(parent);
        if (budget.exhausted) {
            paths.truncated = true;
            code = AH_OK;
            break;
        }
        if ((code == AH_INVALID || code == AH_LIMIT) && ah_local_stopped(call) == AH_OK) {
            skipped++;
            code = AH_OK;
            continue;
        }
        if (code != AH_OK)
            break;
        ah_hash_bytes(source, length, revision);
        ah_hash_update(&hash, paths.items[i], strlen(paths.items[i]) + 1);
        ah_hash_update(&hash, revision, 64);
        scanned++;
        code = find_source(&out, paths.items[i], revision, source, length, query, &pattern, call);
        free(source);
    }
    char revision[65];
    unsigned char partial = paths.truncated;
    ah_hash_update(&hash, &partial, 1);
    ah_hash_finish(&hash, revision);
    bool stale = snap && !yyjson_equals_str(snap, revision);
    yyjson_mut_obj_add_bool(doc, output, "ok", code == AH_OK && !stale);
    yyjson_mut_obj_add_strcpy(doc, output, "snapshot", revision);
    yyjson_mut_obj_add_uint(doc, output, "scanned_files", scanned);
    yyjson_mut_obj_add_uint(doc, output, "skipped_files", skipped);
    yyjson_mut_obj_add_bool(doc, output, "scan_truncated", paths.truncated);
    yyjson_mut_obj_add_uint(doc, output, files ? "total_files" : "total_excerpts", out.total);
    yyjson_mut_obj_add_bool(doc, output, "truncated", offset + out.emitted < out.total);
    if (offset + out.emitted < out.total)
        yyjson_mut_obj_add_uint(doc, output, "next_offset", offset + out.emitted);
    else
        yyjson_mut_obj_add_null(doc, output, "next_offset");
    if (stale) {
        yyjson_mut_obj_remove_str(output, "results");
        yyjson_mut_obj_add_arr(doc, output, "results");
        yyjson_mut_obj_remove_str(output, "next_offset");
        yyjson_mut_obj_add_null(doc, output, "next_offset");
        yyjson_mut_obj_add_str(doc, output, "error", "stale_snapshot");
        yyjson_mut_obj_add_str(doc, output, "message",
                               "Search inputs changed; restart from offset 0");
        result->is_error = true;
    }
    ah_local_paths_free(&paths);
    if (pattern.compiled)
        regfree(&pattern.regex);
    yyjson_doc_free(args);
    if (code != AH_OK || (!out.emitted && out.total > offset && !stale)) {
        ah_mut_doc_free(doc);
        return ah_local_error(
            call, result, error, code == AH_OK ? AH_LIMIT : code,
            code == AH_NOT_FOUND
                ? "Search scope does not exist"
                : "Search incomplete or over budget; narrow the scope or check ignore rules");
    }
    return ah_local_finish(call, result, doc, error);
}
