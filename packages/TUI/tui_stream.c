#include "tui_app.h"
#include <stdlib.h>
#include <string.h>

struct tui_stream {
    struct tui_stream *next;
    uint64_t sequence;
    char *text;
    size_t size, capacity, shown;
    size_t scanned, boundary, fence_width;
    char fence, math;
    bool truncated;
};

enum { STREAM_LIMIT = 4 * 1024 * 1024 };

static tui_stream **stream_slot(tui_app *app, uint64_t sequence) {
    tui_stream **slot = &app->streams;
    while (*slot && (*slot)->sequence != sequence)
        slot = &(*slot)->next;
    return slot;
}

static void stream_free(tui_stream *stream) {
    if (!stream)
        return;
    free(stream->text);
    free(stream);
}

void tui_stream_observe(tui_app *app, const ah_event *event) {
    if (!strcmp(event->type, "model.started")) {
        app->stream_attempt = event->seq;
        return;
    }
    if (strcmp(event->type, "model.delta") || !event->json || !app->stream_attempt)
        return;
    yyjson_doc *doc = yyjson_read(event->json, strlen(event->json), 0);
    yyjson_val *value = yyjson_obj_get(yyjson_doc_get_root(doc), "text");
    size_t length = yyjson_get_len(value);
    const char *text = yyjson_get_str(value);
    if (text && length) {
        pthread_mutex_lock(&app->preview_lock);
        tui_stream **slot = stream_slot(app, app->stream_attempt);
        if (!*slot) {
            *slot = calloc(1, sizeof(**slot));
            if (*slot)
                (*slot)->sequence = app->stream_attempt;
        }
        tui_stream *stream = *slot;
        if (stream && !stream->truncated && length <= STREAM_LIMIT - app->stream_bytes) {
            size_t needed = stream->size + length;
            if (needed > stream->capacity) {
                size_t capacity = stream->capacity ? stream->capacity * 2 : PREVIEW_BYTES;
                if (capacity < needed)
                    capacity = needed;
                char *next = realloc(stream->text, capacity);
                if (next) {
                    stream->text = next;
                    stream->capacity = capacity;
                } else {
                    stream->truncated = true;
                }
            }
            if (!stream->truncated) {
                memcpy(stream->text + stream->size, text, length);
                stream->size = needed;
                app->stream_bytes += length;
            }
        } else if (stream) {
            stream->truncated = true;
        }
        pthread_mutex_unlock(&app->preview_lock);
    }
    yyjson_doc_free(doc);
}

/* A blank line seals a block unless it belongs to a fenced code or display
 * math block. Keep the unfinished block in the redrawable footer. */
static void scan_blocks(tui_stream *stream) {
    const char *text = stream->text;
    char fence = stream->fence, math = stream->math;
    while (stream->scanned < stream->size) {
        const char *start = text + stream->scanned;
        const char *nl = memchr(start, '\n', stream->size - stream->scanned);
        if (!nl)
            break;
        const char *end = nl;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
            --end;
        const char *head = start;
        while (head < end && *head == ' ' && head - start < 3)
            ++head;
        stream->scanned = (size_t)(nl - text) + 1;
        size_t length = (size_t)(end - head);
        if (length && (*head == '\x60' || *head == '~')) {
            size_t run = 1;
            while (run < length && head[run] == *head)
                ++run;
            if (fence && fence == *head && run >= stream->fence_width && run == length)
                fence = 0;
            else if (!fence && !math && run >= 3) {
                fence = *head;
                stream->fence_width = run;
            }
        }
        if (fence)
            continue;
        if (length >= 2 && head[0] == '$' && head[1] == '$') {
            if (math == '$')
                math = 0;
            else if (!math && (length == 2 || memcmp(end - 2, "$$", 2)))
                math = '$';
        } else if (length >= 2 && head[0] == '\\' && head[1] == '[') {
            if (!math && (length == 2 || memcmp(end - 2, "\\]", 2)))
                math = '[';
        } else if (math == '[' && length >= 2 && !memcmp(end - 2, "\\]", 2)) {
            math = 0;
        } else if (math == '$' && length >= 2 && !memcmp(end - 2, "$$", 2)) {
            math = 0;
        }
        if (!length && !math)
            stream->boundary = stream->scanned;
    }
    stream->fence = fence;
    stream->math = math;
}

static size_t plain_boundary(const char *text, size_t size, unsigned cols) {
    size_t width = cols < 16 ? 16 : cols;
    if (size < 3 * width)
        return 0;
    size_t cut = size - 2 * width;
    while (cut > width / 2 && text[cut - 1] != ' ' && text[cut - 1] != '\t')
        --cut;
    if (cut <= width / 2) {
        cut = size - 2 * width;
        while (cut && ((unsigned char)text[cut] & 0xc0) == 0x80)
            --cut;
    }
    return cut;
}

static void print_assistant(tui_app *app, bool first) {
    tui_terminal_clear(&app->tty);
    if (first)
        fputs("\033[38;5;75m \xe2\x80\xa2\033[0m\n", stdout);
}

void tui_stream_render(tui_app *app) {
    app->preview[0] = 0;
    if (!app->stream_active)
        return;
    pthread_mutex_lock(&app->preview_lock);
    tui_stream *stream = *stream_slot(app, app->stream_active);
    char *chunk = NULL;
    size_t cut = 0;
    bool plain = false, first = false;
    if (stream && stream->size > stream->shown) {
        const char *tail = stream->text + stream->shown;
        size_t size = stream->size - stream->shown;
        scan_blocks(stream);
        if (stream->boundary > stream->shown)
            cut = stream->boundary - stream->shown;
        if (!cut && !stream->fence && !stream->math && stream->scanned == stream->shown) {
            cut = plain_boundary(tail, size, app->cols);
            plain = true;
        }
        if (cut && (chunk = malloc(cut))) {
            memcpy(chunk, tail, cut);
            first = !stream->shown;
            stream->shown += cut;
            if (plain)
                stream->scanned = stream->shown;
            tail += cut;
            size -= cut;
        }
        size_t start = size >= PREVIEW_BYTES ? size - PREVIEW_BYTES + 1 : 0;
        while (start < size && ((unsigned char)tail[start] & 0xc0) == 0x80)
            ++start;
        memcpy(app->preview, tail + start, size - start);
        app->preview[size - start] = 0;
    }
    pthread_mutex_unlock(&app->preview_lock);
    if (!chunk)
        return;
    print_assistant(app, first);
    if (plain) {
        tui_safe_text(stdout, chunk, cut);
        fputc('\n', stdout);
    } else {
        tui_markdown_graphics(stdout, chunk, cut, app->cols, &app->graphics);
    }
    free(chunk);
}

static tui_stream *take_active(tui_app *app) {
    pthread_mutex_lock(&app->preview_lock);
    tui_stream **slot = stream_slot(app, app->stream_active);
    tui_stream *stream = *slot;
    if (stream) {
        *slot = stream->next;
        app->stream_bytes -= stream->size;
    }
    pthread_mutex_unlock(&app->preview_lock);
    app->stream_active = 0;
    app->preview[0] = 0;
    return stream;
}

static yyjson_val *text_parts(yyjson_val *item) {
    if (yyjson_equals_str(yyjson_obj_get(item, "type"), "message") &&
        yyjson_equals_str(yyjson_obj_get(item, "role"), "assistant"))
        return yyjson_obj_get(item, "content");
    return NULL;
}

static yyjson_val *part_value(yyjson_val *part) {
    bool refusal = yyjson_equals_str(yyjson_obj_get(part, "type"), "refusal");
    return yyjson_obj_get(part, refusal ? "refusal" : "text");
}

static bool matches_prefix(tui_stream *stream, yyjson_val *output) {
    size_t used = 0;
    size_t i, n, j, count;
    yyjson_val *item, *part;
    yyjson_arr_foreach(output, i, n, item) {
        yyjson_val *parts = text_parts(item);
        yyjson_arr_foreach(parts, j, count, part) {
            yyjson_val *value = part_value(part);
            const char *text = yyjson_get_str(value);
            size_t length = yyjson_get_len(value), needed = stream->shown - used;
            if (!text)
                continue;
            size_t take = length < needed ? length : needed;
            if (memcmp(text, stream->text + used, take))
                return false;
            used += take;
            if (used == stream->shown)
                return true;
        }
    }
    return false;
}

void tui_stream_commit(tui_app *app, yyjson_val *output, bool live) {
    tui_stream *stream = live ? take_active(app) : NULL;
    size_t skip = 0;
    if (stream && stream->shown) {
        if (matches_prefix(stream, output))
            skip = stream->shown;
        else
            tui_line(app, "", "Completed response");
    }
    stream_free(stream);
    bool first = skip == 0;
    size_t i, n, j, count;
    yyjson_val *item, *part;
    yyjson_arr_foreach(output, i, n, item) {
        yyjson_val *parts = text_parts(item);
        yyjson_arr_foreach(parts, j, count, part) {
            yyjson_val *value = part_value(part);
            const char *text = yyjson_get_str(value);
            size_t length = yyjson_get_len(value);
            if (!text)
                continue;
            if (skip >= length) {
                skip -= length;
                continue;
            }
            print_assistant(app, first);
            tui_markdown_graphics(stdout, text + skip, length - skip, app->cols, &app->graphics);
            first = false;
            skip = 0;
        }
    }
}

void tui_stream_abort(tui_app *app, const char *reason) {
    tui_stream *stream = take_active(app);
    if (stream && stream->shown)
        tui_line(app, "! ", reason);
    stream_free(stream);
}

void tui_stream_free(tui_app *app) {
    while (app->streams) {
        tui_stream *stream = app->streams;
        app->streams = stream->next;
        stream_free(stream);
    }
    app->stream_bytes = 0;
}
