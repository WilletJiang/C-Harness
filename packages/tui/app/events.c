#include "app.h"
#include <stdlib.h>
#include <string.h>

#define T_RST      "\033[0m"
#define T_BOLD     "\033[1m"
#define T_STRONG   "\033[1m"
#define T_TEXT     ""
#define T_DIM      "\033[2m"
#define T_FAINT    "\033[2m"
#define T_ACCENT   "\033[38;5;208m"
#define T_AMBER    "\033[38;5;208m"
#define T_RED      "\033[38;5;167m"

static const char *field(yyjson_val *value, const char *key) {
    const char *text = yyjson_get_str(yyjson_obj_get(value, key));
    return text ? text : "";
}

static bool type_is(const ah_event *event, const char *type) {
    return !strcmp(event->type, type);
}

static tui_tool_entry *tool_find(tui_app *app, const char *op_id) {
    if (!op_id || !*op_id)
        return NULL;
    for (size_t i = 0; i < TOOL_CACHE_MAX; ++i)
        if (app->tool_cache[i].id[0] && !strcmp(app->tool_cache[i].id, op_id))
            return &app->tool_cache[i];
    return NULL;
}

static tui_tool_entry *tool_upsert(tui_app *app, const char *op_id) {
    tui_tool_entry *e = tool_find(app, op_id);
    if (e)
        return e;
    e = &app->tool_cache[app->tool_cache_next % TOOL_CACHE_MAX];
    ++app->tool_cache_next;
    memset(e, 0, sizeof(*e));
    if (op_id)
        snprintf(e->id, sizeof(e->id), "%s", op_id);
    return e;
}

static void single_line_copy(char *dst, size_t cap, const char *src, size_t max_chars) {
    if (!cap)
        return;
    size_t w = 0;
    if (src) {
        while (*src && (*src == ' ' || *src == '\t' || *src == '\n' || *src == '\r'))
            ++src;
        bool space = false;
        for (; *src && w + 1 < cap && w < max_chars; ++src) {
            unsigned char c = (unsigned char)*src;
            if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
                if (!space && w) {
                    dst[w++] = ' ';
                    space = true;
                }
            } else if (c >= 32) {
                dst[w++] = (char)c;
                space = false;
            }
        }
    }
    while (w && dst[w - 1] == ' ')
        --w;
    dst[w] = 0;
}

static const char *tool_verb(const char *name) {
    if (!name || !*name)
        return "call";
    if (!strcmp(name, "read_file"))
        return "read";
    if (!strcmp(name, "write_file"))
        return "write";
    if (!strcmp(name, "apply_changes"))
        return "edit";
    if (!strcmp(name, "exec"))
        return "run";
    if (!strcmp(name, "web_run"))
        return "search";
    if (!strcmp(name, "run_checks"))
        return "check";
    if (!strcmp(name, "view_image"))
        return "view";
    return name;
}

static void infer_name_from_args(yyjson_val *val, char name[96]) {
    if (name[0] || !val)
        return;
    if (yyjson_is_str(val)) {
        const char *s = yyjson_get_str(val);
        size_t len = yyjson_get_len(val);
        if (s && len && s[0] == '{') {
            yyjson_doc *sub = yyjson_read(s, len, 0);
            if (sub) {
                infer_name_from_args(yyjson_doc_get_root(sub), name);
                yyjson_doc_free(sub);
            }
        }
        return;
    }
    if (!yyjson_is_obj(val))
        return;
    if (yyjson_obj_get(val, "argv") || yyjson_obj_get(val, "command") || yyjson_obj_get(val, "cmd"))
        snprintf(name, 96, "exec");
    else if (yyjson_obj_get(val, "files"))
        snprintf(name, 96, "apply_changes");
    else if (yyjson_obj_get(val, "queries"))
        snprintf(name, 96, "web_run");
    else if (yyjson_obj_get(val, "query") || yyjson_obj_get(val, "mode") ||
             yyjson_obj_get(val, "glob"))
        snprintf(name, 96, "find");
    else if (yyjson_obj_get(val, "path") && yyjson_obj_get(val, "content"))
        snprintf(name, 96, "write_file");
    else if (yyjson_obj_get(val, "path"))
        snprintf(name, 96, "read_file");
}

static void infer_name_from_result(const char *json, char name[96]) {
    if (name[0] || !json || !*json)
        return;
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (yyjson_is_obj(root)) {
        if (yyjson_obj_get(root, "start_line") || yyjson_obj_get(root, "file_bytes"))
            snprintf(name, 96, "read_file");
        else if (yyjson_obj_get(root, "applied_files") || yyjson_obj_get(root, "application"))
            snprintf(name, 96, "apply_changes");
        else if (yyjson_obj_get(root, "total_excerpts") || yyjson_obj_get(root, "total_files") ||
                 yyjson_obj_get(root, "scanned_files"))
            snprintf(name, 96, "find");
        else if (yyjson_obj_get(root, "exit_code") || yyjson_obj_get(root, "stdout") ||
                 yyjson_obj_get(root, "stderr"))
            snprintf(name, 96, "exec");
        else if (yyjson_obj_get(root, "bytes") && !strcmp(field(root, "status"), "applied"))
            snprintf(name, 96, "write_file");
        else if (yyjson_is_arr(yyjson_obj_get(root, "results")))
            snprintf(name, 96, "web_run");
    }
    yyjson_doc_free(doc);
}

static size_t count_lines_str(const char *s, size_t n) {
    if (!s || !n)
        return 0;
    size_t lines = 1;
    for (size_t i = 0; i < n; ++i)
        if (s[i] == '\n' && i + 1 < n)
            ++lines;
    return lines;
}

static void format_bytes(uint64_t b, char *out, size_t cap) {
    if (b < 1024)
        snprintf(out, cap, "%llu B", (unsigned long long)b);
    else if (b < 1024 * 1024)
        snprintf(out, cap, "%.1f KB", (double)b / 1024.0);
    else
        snprintf(out, cap, "%.1f MB", (double)b / (1024.0 * 1024.0));
}

static void join_str_array(yyjson_val *arr, const char *sep, char *out, size_t cap) {
    out[0] = 0;
    size_t idx, max, used = 0;
    yyjson_val *el;
    yyjson_arr_foreach(arr, idx, max, el) {
        const char *s = yyjson_is_str(el) ? yyjson_get_str(el) : field(el, "query");
        if (!s || !*s)
            continue;
        char clean[96];
        single_line_copy(clean, sizeof(clean), s, 72);
        if (!clean[0])
            continue;
        int n = snprintf(out + used, cap - used, "%s%s", used ? sep : "", clean);
        if (n <= 0 || (size_t)n >= cap - used)
            break;
        used += (size_t)n;
    }
}

static void summarize_args_val(yyjson_val *val, char *out, size_t cap, char *stats, size_t scap) {
    out[0] = 0;
    if (stats && scap)
        stats[0] = 0;
    if (!val)
        return;
    if (yyjson_is_str(val)) {
        const char *s = yyjson_get_str(val);
        size_t len = yyjson_get_len(val);
        if (s && len && s[0] == '{') {
            yyjson_doc *sub = yyjson_read(s, len, 0);
            if (sub && yyjson_is_obj(yyjson_doc_get_root(sub))) {
                summarize_args_val(yyjson_doc_get_root(sub), out, cap, stats, scap);
                yyjson_doc_free(sub);
                return;
            }
            yyjson_doc_free(sub);
        }
        single_line_copy(out, cap, s, 96);
        return;
    }
    if (!yyjson_is_obj(val))
        return;
    // exec argv: ["cmake", "--build", ...]
    yyjson_val *argv = yyjson_obj_get(val, "argv");
    if (yyjson_is_arr(argv)) {
        join_str_array(argv, " ", out, cap);
        if (out[0])
            return;
    }
    // apply_changes files: [{"path":"...","edits":[{"old_text":...,"new_text":...}]}]
    yyjson_val *files = yyjson_obj_get(val, "files");
    if (yyjson_is_arr(files) && yyjson_arr_size(files)) {
        size_t fcount = yyjson_arr_size(files), added = 0, removed = 0;
        const char *first_path = field(yyjson_arr_get_first(files), "path");
        if (fcount == 1 && *first_path)
            single_line_copy(out, cap, first_path, 96);
        else if (*first_path)
            snprintf(out, cap, "%s (+%zu files)", first_path, fcount - 1);
        size_t fi, fm, ei, em;
        yyjson_val *fobj, *ed;
        yyjson_arr_foreach(files, fi, fm, fobj) {
            yyjson_val *edits = yyjson_obj_get(fobj, "edits");
            yyjson_arr_foreach(edits, ei, em, ed) {
                yyjson_val *ot = yyjson_obj_get(ed, "old_text");
                yyjson_val *nt = yyjson_obj_get(ed, "new_text");
                removed += count_lines_str(yyjson_get_str(ot), yyjson_get_len(ot));
                added += count_lines_str(yyjson_get_str(nt), yyjson_get_len(nt));
            }
        }
        if (stats && scap && (added || removed))
            snprintf(stats, scap, "+%zu -%zu", added, removed);
        if (out[0])
            return;
    }
    // web_run queries
    yyjson_val *queries = yyjson_obj_get(val, "queries");
    if (yyjson_is_arr(queries) && yyjson_arr_size(queries)) {
        join_str_array(queries, ", ", out, cap);
        if (stats && scap && yyjson_arr_size(queries) > 1)
            snprintf(stats, scap, "%zu queries", yyjson_arr_size(queries));
        if (out[0])
            return;
    }
    static const char *const priority[] = {
        "command", "cmd", "path", "file_path", "file", "query", "pattern",
        "url", "question", "task", "target", "dir", "directory", "glob", "name", "id",
    };
    for (size_t i = 0; i < sizeof(priority) / sizeof(*priority); ++i) {
        yyjson_val *v = yyjson_obj_get(val, priority[i]);
        if (yyjson_is_str(v)) {
            single_line_copy(out, cap, yyjson_get_str(v), 100);
            if (out[0])
                return;
        }
    }
    size_t idx, max, shown = 0, used = 0;
    yyjson_val *k, *v;
    yyjson_obj_foreach(val, idx, max, k, v) {
        if (shown >= 2 || used + 12 >= cap)
            break;
        const char *ks = yyjson_get_str(k);
        if (!ks)
            continue;
        char valbuf[64] = "";
        if (yyjson_is_str(v))
            single_line_copy(valbuf, sizeof(valbuf), yyjson_get_str(v), 40);
        else if (yyjson_is_int(v))
            snprintf(valbuf, sizeof(valbuf), "%lld", (long long)yyjson_get_sint(v));
        else if (yyjson_is_uint(v))
            snprintf(valbuf, sizeof(valbuf), "%llu", (unsigned long long)yyjson_get_uint(v));
        else if (yyjson_is_bool(v))
            snprintf(valbuf, sizeof(valbuf), "%s", yyjson_get_bool(v) ? "true" : "false");
        else
            continue;
        if (!valbuf[0])
            continue;
        int n = snprintf(out + used, cap - used, "%s%s=%s", shown ? "  " : "", ks, valbuf);
        if (n > 0)
            used += (size_t)n < cap - used ? (size_t)n : cap - used - 1;
        ++shown;
    }
}

static void format_pending_args(yyjson_val *args, char *out, size_t cap) {
    out[0] = 0;
    if (!args || !cap)
        return;
    if (yyjson_is_str(args)) {
        const char *s = yyjson_get_str(args);
        size_t len = yyjson_get_len(args);
        if (s && len && s[0] == '{') {
            yyjson_doc *sub = yyjson_read(s, len, 0);
            if (sub && yyjson_is_obj(yyjson_doc_get_root(sub))) {
                format_pending_args(yyjson_doc_get_root(sub), out, cap);
                yyjson_doc_free(sub);
                return;
            }
            yyjson_doc_free(sub);
        }
        single_line_copy(out, cap, s, 220);
        return;
    }
    if (!yyjson_is_obj(args))
        return;
    yyjson_val *argv = yyjson_obj_get(args, "argv");
    if (yyjson_is_arr(argv)) {
        char cmd[220];
        join_str_array(argv, " ", cmd, sizeof(cmd));
        const char *cwd = field(args, "cwd");
        if (cmd[0] && *cwd)
            snprintf(out, cap, "command: %s\ncwd: %s", cmd, cwd);
        else if (cmd[0])
            snprintf(out, cap, "command: %s", cmd);
        if (out[0])
            return;
    }
    yyjson_val *files = yyjson_obj_get(args, "files");
    if (yyjson_is_arr(files) && yyjson_arr_size(files)) {
        char target[160], diff[48];
        summarize_args_val(args, target, sizeof(target), diff, sizeof(diff));
        if (target[0] && diff[0])
            snprintf(out, cap, "files: %s\nlines: %s", target, diff);
        else if (target[0])
            snprintf(out, cap, "files: %s", target);
        if (out[0])
            return;
    }
    size_t idx, max, shown = 0, used = 0;
    yyjson_val *k, *v;
    yyjson_obj_foreach(args, idx, max, k, v) {
        if (shown >= 3 || used + 16 >= cap)
            break;
        const char *ks = yyjson_get_str(k);
        if (!ks)
            continue;
        char valbuf[160] = "";
        if (yyjson_is_str(v))
            single_line_copy(valbuf, sizeof(valbuf), yyjson_get_str(v), 120);
        else if (yyjson_is_int(v))
            snprintf(valbuf, sizeof(valbuf), "%lld", (long long)yyjson_get_sint(v));
        else if (yyjson_is_uint(v))
            snprintf(valbuf, sizeof(valbuf), "%llu", (unsigned long long)yyjson_get_uint(v));
        else if (yyjson_is_bool(v))
            snprintf(valbuf, sizeof(valbuf), "%s", yyjson_get_bool(v) ? "true" : "false");
        else {
            char *compact = yyjson_val_write(v, 0, NULL);
            if (compact) {
                single_line_copy(valbuf, sizeof(valbuf), compact, 100);
                free(compact);
            }
        }
        if (!valbuf[0])
            continue;
        int n = snprintf(out + used, cap - used, "%s%s: %s", shown ? "\n" : "", ks, valbuf);
        if (n > 0)
            used += (size_t)n < cap - used ? (size_t)n : cap - used - 1;
        ++shown;
    }
}

typedef struct {
    bool is_error;
    char metric[128];
    char tail[3][180];
    unsigned tail_count;
    size_t total_lines;
} tool_receipt_info;

static void collect_tail_lines(const char *src, tool_receipt_info *info) {
    if (!src || !*src)
        return;
    const char *p = src;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        while (len && (p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == '\t'))
            --len;
        const char *s = p;
        while (len && (*s == ' ' || *s == '\t'))
            ++s, --len;
        if (len) {
            ++info->total_lines;
            char line[180];
            size_t copy = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
            memcpy(line, s, copy);
            line[copy] = 0;
            single_line_copy(line, sizeof(line), line, sizeof(line) - 1);
            if (line[0]) {
                if (info->tail_count < 3) {
                    snprintf(info->tail[info->tail_count++], sizeof(info->tail[0]), "%s", line);
                } else {
                    memmove(info->tail[0], info->tail[1], sizeof(info->tail[0]) * 2);
                    snprintf(info->tail[2], sizeof(info->tail[2]), "%s", line);
                }
            }
        }
        if (!nl)
            break;
        p = nl + 1;
    }
}

static void summarize_tool_result(const char *name, const char *arg_stats, const char *json,
                                  tool_receipt_info *info) {
    memset(info, 0, sizeof(*info));
    if (!json || !*json) {
        if (arg_stats && *arg_stats)
            snprintf(info->metric, sizeof(info->metric), "%s", arg_stats);
        return;
    }
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!root) {
        single_line_copy(info->metric, sizeof(info->metric), json, 80);
        yyjson_doc_free(doc);
        return;
    }
    if (yyjson_is_str(root)) {
        single_line_copy(info->metric, sizeof(info->metric), yyjson_get_str(root), 80);
        yyjson_doc_free(doc);
        return;
    }
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return;
    }
    const char *err = field(root, "error");
    const char *msg = field(root, "message");
    if (yyjson_is_false(yyjson_obj_get(root, "ok")) || *err)
        info->is_error = true;

    yyjson_val *exit_val = yyjson_obj_get(root, "exit_code");
    bool has_exit = yyjson_is_int(exit_val) || yyjson_is_uint(exit_val);
    long long exit_code = has_exit ? yyjson_get_sint(exit_val) : 0;
    if (has_exit && exit_code != 0)
        info->is_error = true;

    bool is_read = name && !strcmp(name, "read_file");
    bool is_write = name && !strcmp(name, "write_file");
    bool is_edit = name && !strcmp(name, "apply_changes");
    bool is_find = name && !strcmp(name, "find");
    bool is_exec = has_exit || (name && (!strcmp(name, "exec") || !strcmp(name, "run_checks")));

    if (info->is_error) {
        if (has_exit)
            snprintf(info->metric, sizeof(info->metric), "exit %lld", exit_code);
        else if (*err)
            snprintf(info->metric, sizeof(info->metric), "%s", err);
        const char *se = field(root, "stderr");
        const char *so = field(root, "stdout");
        if (*se)
            collect_tail_lines(se, info);
        if (!info->tail_count && *so)
            collect_tail_lines(so, info);
        if (!info->tail_count && *msg)
            collect_tail_lines(msg, info);
        if (!info->tail_count && *err && has_exit)
            collect_tail_lines(err, info);
        yyjson_doc_free(doc);
        return;
    }

    if (is_read) {
        uint64_t start = yyjson_get_uint(yyjson_obj_get(root, "start_line"));
        uint64_t lines = yyjson_get_uint(yyjson_obj_get(root, "lines"));
        uint64_t bytes = yyjson_get_uint(yyjson_obj_get(root, "bytes"));
        char sz[32];
        format_bytes(bytes, sz, sizeof(sz));
        if (start && lines)
            snprintf(info->metric, sizeof(info->metric), "L%llu-%llu  %s",
                     (unsigned long long)start, (unsigned long long)(start + lines - 1), sz);
        else if (lines)
            snprintf(info->metric, sizeof(info->metric), "%llu lines  %s",
                     (unsigned long long)lines, sz);
        else
            snprintf(info->metric, sizeof(info->metric), "%s", sz);
    } else if (is_write) {
        uint64_t bytes = yyjson_get_uint(yyjson_obj_get(root, "bytes"));
        format_bytes(bytes, info->metric, sizeof(info->metric));
    } else if (is_edit) {
        uint64_t applied = yyjson_get_uint(yyjson_obj_get(root, "applied_files"));
        if (arg_stats && *arg_stats && applied > 1)
            snprintf(info->metric, sizeof(info->metric), "%llu files  %s",
                     (unsigned long long)applied, arg_stats);
        else if (arg_stats && *arg_stats)
            snprintf(info->metric, sizeof(info->metric), "%s", arg_stats);
        else if (applied)
            snprintf(info->metric, sizeof(info->metric), "%llu %s", (unsigned long long)applied,
                     applied == 1 ? "file" : "files");
    } else if (is_find) {
        yyjson_val *tf = yyjson_obj_get(root, "total_files");
        yyjson_val *te = yyjson_obj_get(root, "total_excerpts");
        uint64_t scanned = yyjson_get_uint(yyjson_obj_get(root, "scanned_files"));
        if (yyjson_is_uint(te) || yyjson_is_int(te))
            snprintf(info->metric, sizeof(info->metric), "%llu hits  %llu scanned",
                     (unsigned long long)yyjson_get_uint(te), (unsigned long long)scanned);
        else if (yyjson_is_uint(tf) || yyjson_is_int(tf))
            snprintf(info->metric, sizeof(info->metric), "%llu files",
                     (unsigned long long)yyjson_get_uint(tf));
    } else if (is_exec) {
        snprintf(info->metric, sizeof(info->metric), "exit %lld", exit_code);
        const char *so = field(root, "stdout");
        const char *se = field(root, "stderr");
        if (*so)
            collect_tail_lines(so, info);
        if (!info->tail_count && *se)
            collect_tail_lines(se, info);
    } else {
        yyjson_val *results = yyjson_obj_get(root, "results");
        yyjson_val *matches = yyjson_obj_get(root, "matches");
        yyjson_val *files = yyjson_obj_get(root, "files");
        if (arg_stats && *arg_stats)
            snprintf(info->metric, sizeof(info->metric), "%s", arg_stats);
        else if (yyjson_is_arr(results))
            snprintf(info->metric, sizeof(info->metric), "%zu results", yyjson_arr_size(results));
        else if (yyjson_is_arr(matches))
            snprintf(info->metric, sizeof(info->metric), "%zu matches", yyjson_arr_size(matches));
        else if (yyjson_is_arr(files))
            snprintf(info->metric, sizeof(info->metric), "%zu files", yyjson_arr_size(files));
        else if (*msg)
            single_line_copy(info->metric, sizeof(info->metric), msg, 64);
    }
    yyjson_doc_free(doc);
}

static void refresh_active_tool_label(tui_app *app) {
    app->active_tool[0] = 0;
    app->active_tools = 0;
    const tui_tool_entry *latest = NULL;
    for (size_t i = 0; i < TOOL_CACHE_MAX; ++i) {
        if (app->tool_cache[i].active && strcmp(app->tool_cache[i].name, "flow")) {
            ++app->active_tools;
            if (!latest || app->tool_cache[i].started_ms >= latest->started_ms)
                latest = &app->tool_cache[i];
        }
    }
    if (latest) {
        const char *verb = tool_verb(latest->name);
        if (latest->summary[0])
            snprintf(app->active_tool, sizeof(app->active_tool), "%-6s  %s", verb,
                     latest->summary);
        else
            snprintf(app->active_tool, sizeof(app->active_tool), "%s", verb);
    }
}

static void print_clipped(const char *s, size_t max_bytes) {
    if (!s || !*s || !max_bytes)
        return;
    size_t len = strlen(s);
    if (len <= max_bytes) {
        tui_safe_text(stdout, s, len);
        return;
    }
    size_t cut = max_bytes > 1 ? max_bytes - 1 : 0;
    while (cut && ((unsigned char)s[cut] & 0xc0) == 0x80)
        --cut;
    tui_safe_text(stdout, s, cut);
    fputs("\xe2\x80\xa6", stdout);
}

void tui_block_begin(tui_app *app, tui_block_kind kind) {
    tui_terminal_clear(&app->tty);
    if (app->last_block != TUI_BLOCK_NONE) {
        bool need_gap = kind == TUI_BLOCK_USER || app->last_block != kind;
        if (need_gap)
            fputc('\n', stdout);
    }
    app->last_block = kind;
}

static void print_tool_receipt(tui_app *app, const char *name, const char *summary,
                               const tool_receipt_info *info, const char *badge, bool is_muted) {
    tui_block_begin(app, TUI_BLOCK_TOOL);
    unsigned cols = app->cols ? app->cols : 80;
    const char *verb = tool_verb(name);
    size_t vlen = strlen(verb);
    size_t blen = badge ? strlen(badge) : 0;
    size_t mlen = info->metric[0] ? strlen(info->metric) : 0;
    size_t verb_col = vlen > 6 ? vlen : 6;
    size_t budget = cols > verb_col + blen + mlen + 14 ? cols - verb_col - blen - mlen - 14 : 16;

    if (info->is_error)
        fputs(T_RED "! " T_RST, stdout);
    else if (is_muted)
        fputs(T_AMBER "- " T_RST, stdout);
    else
        fputs("  ", stdout);

    fputs(info->is_error ? T_RED : T_DIM, stdout);
    tui_safe_text(stdout, verb, vlen);
    for (size_t pad = vlen; pad < 6; ++pad)
        fputc(' ', stdout);

    if (summary && *summary) {
        fputs(T_RST "  " T_TEXT, stdout);
        print_clipped(summary, budget);
    }
    if (mlen) {
        fputs(T_RST "   ", stdout);
        fputs(info->is_error ? T_RED : T_DIM, stdout);
        tui_safe_text(stdout, info->metric, mlen);
    }
    if (blen) {
        fputs(T_RST "   " T_AMBER, stdout);
        tui_safe_text(stdout, badge, blen);
    }
    fputs(T_RST "\n", stdout);

    if (info->tail_count) {
        size_t line_budget = cols > 12 ? cols - 12 : 40;
        if (info->total_lines > info->tail_count)
            fprintf(stdout, "    " T_FAINT "\xe2\x94\x82 \xe2\x80\xa6 +%zu lines" T_RST "\n",
                    info->total_lines - info->tail_count);
        for (unsigned i = 0; i < info->tail_count; ++i) {
            fputs("    " T_FAINT "\xe2\x94\x82 " T_RST, stdout);
            fputs(info->is_error ? T_RED : T_DIM, stdout);
            print_clipped(info->tail[i], line_budget);
            fputs(T_RST "\n", stdout);
        }
    }
    fflush(stdout);
}

void tui_print_banner(tui_app *app) {
    tui_terminal_clear(&app->tty);
    app->last_block = TUI_BLOCK_NONE;
    fputs("  " T_STRONG "ah" T_RST " " T_FAINT "v" AH_VERSION, stdout);
    if (app->model[0]) {
        fputs(T_RST "   " T_TEXT, stdout);
        tui_safe_text(stdout, app->model, strlen(app->model));
    }
    if (app->cwd[0]) {
        fputs(T_RST "   " T_DIM, stdout);
        tui_safe_text(stdout, app->cwd, strlen(app->cwd));
        if (app->branch[0]) {
            fputs(" (", stdout);
            tui_safe_text(stdout, app->branch, strlen(app->branch));
            fputc(')', stdout);
        }
    }
    fputs(T_RST "\n", stdout);
    fflush(stdout);
}

static void network_status(tui_app *app, const char *json) {
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    if (!doc)
        return;
    yyjson_val *data = yyjson_doc_get_root(doc);
    const char *phase = field(data, "phase");
    bool websocket = !strcmp(field(data, "transport"), "websocket");
    const char *label = !strcmp(phase, "connecting") ?
                            (websocket ? "Connecting via WebSocket" : "Connecting to model") :
                        !strcmp(phase, "waiting") ? "Waiting for model output" :
                        !strcmp(phase, "streaming") ? "Receiving model output" :
                        !strcmp(phase, "draining") ? "Verifying response completion" :
                        !strcmp(phase, "refreshing") ? "Refreshing credentials" : "";
    if (!strcmp(phase, "fallback"))
        label = "Switching to HTTP";
    pthread_mutex_lock(&app->preview_lock);
    app->network_permission_denied = !strcmp(field(data, "reason"), "permission");
    if (!strcmp(phase, "backoff"))
        snprintf(app->network_status, sizeof(app->network_status), "Retrying in %llu ms",
                 (unsigned long long)yyjson_get_uint(yyjson_obj_get(data, "retry_in_ms")));
    else
        snprintf(app->network_status, sizeof(app->network_status), "%s", label);
    pthread_mutex_unlock(&app->preview_lock);
    yyjson_doc_free(doc);
    tui_notify(app->wake[1]);
}

void tui_observe(void *context, const ah_event *event) {
    tui_app *app = context;
    bool root = event->session_id && !strcmp(event->session_id, app->session);
    if (root && type_is(event, "network.state")) {
        network_status(app, event->json);
        return;
    }
    if (root)
        tui_stream_observe(app, event);
    if (event->seq)
        atomic_store(&app->journal_dirty, true);
    if (event->seq || (root && type_is(event, "model.delta")))
        tui_notify(app->wake[1]);
}

void tui_show_error(tui_app *app, ah_code code, const ah_error *error) {
    app->status.notice_error = true;
    snprintf(app->notice, sizeof(app->notice), "%s%s%s", ah_code_name(code),
             error->message[0] ? "  " : "", error->message);
}

void tui_line(tui_app *app, const char *prefix, const char *text) {
    if (!strcmp(prefix, "> ")) {
        tui_block_begin(app, TUI_BLOCK_USER);
        fputs("  " T_ACCENT "\xe2\x80\xba " T_RST T_STRONG, stdout);
        tui_safe_text(stdout, text, strlen(text));
        fputs(T_RST "\n", stdout);
    } else if (!strcmp(prefix, "+ ")) {
        tui_block_begin(app, TUI_BLOCK_NOTE);
        fputs("  " T_FAINT "\xe2\x86\xb3 " T_RST T_TEXT, stdout);
        tui_safe_text(stdout, text, strlen(text));
        fputs(T_RST "\n", stdout);
    } else if (!strcmp(prefix, "! ")) {
        tui_block_begin(app, TUI_BLOCK_NOTE);
        fputs("  " T_AMBER "\xe2\x96\x8e " T_RST T_TEXT, stdout);
        tui_safe_text(stdout, text, strlen(text));
        fputs(T_RST "\n", stdout);
    } else {
        tui_block_begin(app, TUI_BLOCK_NOTE);
        fputs("  " T_DIM, stdout);
        fputs(prefix, stdout);
        fputs(T_RST T_TEXT, stdout);
        tui_safe_text(stdout, text, strlen(text));
        fputs(T_RST "\n", stdout);
    }
    fflush(stdout);
}

void tui_show_json(tui_app *app, const char *json) {
    yyjson_doc *doc = json ? yyjson_read(json, strlen(json), 0) : NULL;
    char *pretty = doc ? yyjson_write(doc, YYJSON_WRITE_PRETTY_TWO_SPACES, NULL) : NULL;
    const char *src = pretty ? pretty : (json ? json : "");
    tui_terminal_clear(&app->tty);
    const char *p = src;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        fputs("  " T_FAINT "\xe2\x94\x82 " T_DIM, stdout);
        tui_safe_text(stdout, p, len);
        fputs(T_RST "\n", stdout);
        if (!nl)
            break;
        p = nl + 1;
    }
    fputc('\n', stdout);
    fflush(stdout);
    free(pretty);
    yyjson_doc_free(doc);
}

static void transcript(tui_app *app, const ah_event *event, bool live) {
    bool response = type_is(event, "model.completed");
    bool input = type_is(event, "input.submitted");
    bool steer = type_is(event, "input.steered");
    bool question = type_is(event, "input.question_opened");
    bool answer = type_is(event, "input.question_answered");
    bool intent = type_is(event, "tool.intent");
    bool started = type_is(event, "tool.started");
    bool completed = type_is(event, "tool.completed") || type_is(event, "tool.late_result");
    bool denied = type_is(event, "tool.denied");
    bool interrupted = type_is(event, "tool.interrupted");
    bool unknown = type_is(event, "tool.outcome_unknown");
    if (!response && !input && !steer && !question && !answer && !intent && !started &&
        !completed && !denied && !interrupted && !unknown)
        return;
    yyjson_doc *doc = event->json ? yyjson_read(event->json, strlen(event->json), 0) : NULL;
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (input) {
        yyjson_val *content = yyjson_obj_get(root, "content");
        const char *text = yyjson_get_str(content);
        char summary[PREVIEW_BYTES];
        if (!text) {
            tui_input_summary(content, summary, sizeof(summary));
            text = summary[0] ? summary : "[typed input]";
        }
        tui_line(app, "> ", text);
    } else if (steer) {
        if (!yyjson_is_true(yyjson_obj_get(root, "duplicate")))
            tui_line(app, "> ", field(root, "text"));
    } else if (question) {
        tui_line(app, "? ", field(yyjson_obj_get(root, "arguments"), "question"));
        if (live) {
            app->status.notice_error = false;
            snprintf(app->notice, sizeof(app->notice), "Question available. Use /pending to reply.");
        }
    } else if (answer) {
        const char *text = yyjson_get_str(root);
        tui_line(app, "> ", text ? text : event->json);
    } else if (intent) {
        tui_tool_entry *e = tool_upsert(app, event->operation_id);
        const char *tname = field(root, "name");
        if (*tname)
            snprintf(e->name, sizeof(e->name), "%s", tname);
        yyjson_val *args = yyjson_obj_get(root, "arguments");
        if (!args)
            args = yyjson_obj_get(root, "input");
        summarize_args_val(args, e->summary, sizeof(e->summary), e->stats, sizeof(e->stats));
    } else if (started) {
        tui_tool_entry *e = tool_upsert(app, event->operation_id);
        if (!e->name[0] && root)
            infer_name_from_args(root, e->name);
        if (!e->summary[0] && root)
            summarize_args_val(root, e->summary, sizeof(e->summary), e->stats, sizeof(e->stats));
        e->started_ms = ah_now_ms();
        e->active = true;
        refresh_active_tool_label(app);
    } else if (completed || denied || interrupted || unknown) {
        tui_tool_entry *e = tool_find(app, event->operation_id);
        char inferred[96] = "";
        const char *tname = e && e->name[0] ? e->name : "";
        if (!*tname && completed) {
            infer_name_from_result(event->json, inferred);
            tname = inferred;
        }
        char badge[32] = "";
        if (denied)
            snprintf(badge, sizeof(badge), "denied");
        else if (interrupted)
            snprintf(badge, sizeof(badge), "interrupted");
        else if (unknown)
            snprintf(badge, sizeof(badge), "unknown");
        tool_receipt_info info = {0};
        if (completed)
            summarize_tool_result(tname, e ? e->stats : NULL, event->json, &info);
        // Hide internal flow wrapper unless it failed
        if (strcmp(tname, "flow") || info.is_error || denied || interrupted || unknown)
            print_tool_receipt(app, tname, e ? e->summary : "", &info, badge,
                               denied || interrupted || unknown);
        if (e) {
            e->active = false;
            refresh_active_tool_label(app);
        }
    } else {
        tui_stream_commit(app, yyjson_obj_get(root, "output"), live);
    }
    yyjson_doc_free(doc);
}

static void history(void *context, const ah_event *event) {
    transcript(context, event, false);
}

static void journal(void *context, const ah_event *event) {
    tui_app *app = context;
    bool root = event->session_id && !strcmp(event->session_id, app->session);
    if (root) {
        if (type_is(event, "model.started"))
            app->stream_active = event->seq;
        else if (type_is(event, "model.interrupted"))
            tui_stream_abort(app, "Response interrupted by steering");
        transcript(app, event, true);
    }
    if (event->session_id && event->operation_id &&
        (type_is(event, "approval.requested") || type_is(event, "input.requested"))) {
        snprintf(app->pending_session, sizeof(app->pending_session), "%s", event->session_id);
        snprintf(app->operation, sizeof(app->operation), "%s", event->operation_id);
        const char *kind = type_is(event, "approval.requested") ? "approval" : "input";
        snprintf(app->pending_status, sizeof(app->pending_status), "%s", kind);
        app->pending_background = false;
        tui_tool_entry *e = tool_find(app, event->operation_id);
        snprintf(app->pending_tool, sizeof(app->pending_tool), "%s",
                 e && e->name[0] ? e->name : kind);
        if (event->json) {
            yyjson_doc *pdoc = yyjson_read(event->json, strlen(event->json), 0);
            yyjson_val *proot = yyjson_doc_get_root(pdoc);
            if (!strcmp(kind, "input")) {
                const char *q = field(proot, "question");
                if (*q)
                    snprintf(app->pending_summary, sizeof(app->pending_summary), "%s", q);
                else
                    format_pending_args(proot, app->pending_summary, sizeof(app->pending_summary));
            } else {
                format_pending_args(proot, app->pending_summary, sizeof(app->pending_summary));
            }
            yyjson_doc_free(pdoc);
        }
        app->status.notice_error = false;
        app->notice[0] = 0; // Inline pending card renders this cleanly above the composer
    }
    if (root && type_is(event, "input.rejected"))
        tui_line(app, "! ", "steering rejected");
    else if (root && type_is(event, "input.reply_rejected"))
        tui_line(app, "! ", "That question no longer accepts this answer");
}

ah_code tui_snapshot(tui_app *app, const char *session, bool set_cursor, ah_error *error) {
    char *json = NULL;
    ah_code code = ah_reader_session_json(app->reader, session, &json, error);
    if (code != AH_OK)
        return code;
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    ah_free(json);
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!root) {
        error->code = AH_IO;
        snprintf(error->message, sizeof(error->message), "Cannot decode session snapshot");
        return AH_IO;
    }
    if (!strcmp(session, app->session)) {
        snprintf(app->state, sizeof(app->state), "%s", field(root, "state"));
        const char *mdl = field(root, "model");
        if (*mdl)
            snprintf(app->model, sizeof(app->model), "%s", mdl);
        app->turn = (unsigned)yyjson_get_uint(yyjson_obj_get(root, "turn"));
    }
    if (set_cursor)
        app->cursor = yyjson_get_uint(yyjson_obj_get(root, "cursor"));
    yyjson_val *pending = yyjson_obj_get(root, "pending");
    const char *status = field(pending, "status");
    if (!strcmp(status, "approval") || !strcmp(status, "input")) {
        snprintf(app->pending_session, sizeof(app->pending_session), "%s", session);
        snprintf(app->operation, sizeof(app->operation), "%s", field(pending, "operation_id"));
        snprintf(app->pending_status, sizeof(app->pending_status), "%s", status);
        app->pending_background = yyjson_is_true(yyjson_obj_get(pending, "background"));
        app->pending_ask = !strcmp(status, "input") &&
                           tui_ask_has_options(yyjson_obj_get(pending, "arguments"));
        snprintf(app->pending_tool, sizeof(app->pending_tool), "%s", field(pending, "name"));
        yyjson_val *args = yyjson_obj_get(pending, "arguments");
        const char *question = field(args, "question");
        if (!strcmp(status, "input") && *question)
            snprintf(app->pending_summary, sizeof(app->pending_summary), "%s", question);
        else
            format_pending_args(args, app->pending_summary, sizeof(app->pending_summary));
        app->status.notice_error = false;
        app->notice[0] = 0;
    } else if (!strcmp(session, app->pending_session)) {
        app->pending_ask = false;
        app->pending_background = false;
        app->operation[0] = app->pending_status[0] = app->pending_session[0] = 0;
        app->pending_tool[0] = app->pending_summary[0] = 0;
    }
    yyjson_doc_free(doc);
    return AH_OK;
}

ah_code tui_history_page(tui_app *app, uint64_t boundary, ah_error *error) {
    uint64_t first = 0;
    ah_code code = ah_reader_recent_events(app->reader, app->session, boundary, HISTORY_PAGE,
                                           history, app, &first, error);
    if (code == AH_OK)
        app->history = first;
    return code;
}

void tui_restore_history(tui_app *app) {
    if (!app->restore_history)
        return;
    app->restore_history = false;
    ah_error error = {0};
    ah_code code = tui_history_page(app, app->cursor + 1, &error);
    if (code != AH_OK)
        tui_show_error(app, code, &error);
}

void tui_refresh(tui_app *app) {
    uint64_t last;
    ah_error error = {0};
    ah_code code = ah_reader_events(app->reader, NULL, app->cursor, LIVE_PAGE, journal, app, &last,
                                    &error);
    app->refresh_events = code == AH_BUSY || (code == AH_OK && last > app->cursor);
    if (code == AH_LIMIT) {
        tui_show_error(app, code, &error);
        (void)tui_snapshot(app, app->session, true, &error);
    } else if (code != AH_OK && code != AH_BUSY) {
        tui_show_error(app, code, &error);
    } else if (code == AH_OK) {
        app->cursor = last;
    }
}
