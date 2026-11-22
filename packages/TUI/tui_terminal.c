#include "tui_internal.h"
#include <curses.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <term.h>
#undef lines
#include <unistd.h>
#include <utf8proc.h>

#define TUI_PASTE_LIMIT (64 * 1024)

static char *capability(char *name) {
    char *value = tigetstr(name);
    return value == (char *)-1 ? NULL : value;
}

bool tui_terminal_open(tui_terminal *tty) {
    int status;
    static char cuu1[] = "cuu1", ed[] = "ed";
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) ||
        tcgetattr(STDIN_FILENO, &tty->saved) != 0 ||
        setupterm(NULL, STDOUT_FILENO, &status) != OK ||
        !(tty->up = capability(cuu1)) || !(tty->erase = capability(ed)))
        return false;
    struct termios mode = tty->saved;
    mode.c_lflag &= ~(ICANON | ECHO | IEXTEN | ISIG);
    mode.c_iflag &= ~(ICRNL | INLCR | IGNCR | IXON | ISTRIP);
    mode.c_cc[VMIN] = 1;
    mode.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &mode) != 0)
        return false;
    tty->active = true;
    fputs("\033[?2004h", stdout);
    return fflush(stdout) == 0;
}

void tui_terminal_size(unsigned *cols, unsigned *rows) {
    struct winsize size = {0};
    (void)ioctl(STDOUT_FILENO, TIOCGWINSZ, &size);
    *cols = size.ws_col ? size.ws_col : 80;
    *rows = size.ws_row ? size.ws_row : 24;
}

void tui_terminal_clear(tui_terminal *tty) {
    if (!tty->frame)
        return;
    putchar('\r');
    for (unsigned i = 0; i < tty->cursor_row; ++i)
        tputs(tty->up, 1, putchar);
    tputs(tty->erase, 1, putchar);
    tty->frame = false;
}

void tui_terminal_close(tui_terminal *tty) {
    if (tty->active) {
        tui_terminal_clear(tty);
        fputs("\033[?25h\033[?2004l\033[0m", stdout);
        fflush(stdout);
    }
    if (cur_term)
        del_curterm(cur_term);
    if (tty->active)
        (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &tty->saved);
}

void tui_safe_text(FILE *out, const char *text, size_t size) {
    while (size) {
        utf8proc_int32_t codepoint;
        utf8proc_ssize_t n = utf8proc_iterate((const unsigned char *)text,
                                              (utf8proc_ssize_t)(size < 4 ? size : 4), &codepoint);
        if (n < 0) {
            fputs("\xef\xbf\xbd", out);
            n = 1;
        } else if ((codepoint < 32 && codepoint != '\n' && codepoint != '\t') ||
                   (codepoint >= 127 && codepoint <= 159)) {
            fputc(' ', out);
        } else {
            fwrite(text, 1, (size_t)n, out);
        }
        size -= (size_t)n;
        text += n;
    }
}

// Emit a line allowing only validated SGR escapes (\033[[0-9;]*m); all other bytes are sanitized.
static void draw_styled_line(FILE *out, const char *text, size_t size) {
    size_t i = 0;
    while (i < size) {
        if (text[i] == '\033' && i + 2 < size && text[i + 1] == '[') {
            size_t j = i + 2;
            while (j < size && j - i < 32 &&
                   ((text[j] >= '0' && text[j] <= '9') || text[j] == ';'))
                ++j;
            if (j < size && text[j] == 'm') {
                fwrite(text + i, 1, j - i + 1, out);
                i = j + 1;
                continue;
            }
        }
        size_t start = i++;
        while (i < size && text[i] != '\033')
            ++i;
        tui_safe_text(out, text + start, i - start);
    }
}

void tui_terminal_draw(tui_terminal *tty, const tui_frame *frame) {
    fputs("\033[?2026h\033[?25l", stdout);
    tui_terminal_clear(tty);
    const char *line = frame->text;
    for (const char *end = strchr(line, '\n'); end; end = strchr(line, '\n')) {
        draw_styled_line(stdout, line, (size_t)(end - line));
        fputs("\033[0m\r\n", stdout);
        line = end + 1;
    }
    draw_styled_line(stdout, line, strlen(line));
    fputs("\033[0m\r", stdout);
    for (unsigned i = frame->cursor_row + 1; i < frame->lines; ++i)
        tputs(tty->up, 1, putchar);
    if (frame->cursor_col)
        fprintf(stdout, "\033[%uC", frame->cursor_col);
    if (!frame->hide_cursor)
        fputs("\033[?25h", stdout);
    fputs("\033[?2026l", stdout);
    tty->cursor_row = frame->cursor_row;
    tty->frame = true;
    fflush(stdout);
}

static void paste_append(tui_input *input, char c) {
    if (input->paste_overflow)
        return;
    if (input->paste_len == TUI_PASTE_LIMIT) {
        input->paste_overflow = true;
        return;
    }
    if (input->paste_len == input->paste_capacity) {
        size_t capacity = input->paste_capacity ? input->paste_capacity * 2 : 1024;
        char *buffer = realloc(input->pasted, capacity);
        if (!buffer) {
            input->paste_overflow = true;
            return;
        }
        input->pasted = buffer;
        input->paste_capacity = capacity;
    }
    input->pasted[input->paste_len++] = c;
}

static void paste_byte(tui_input *input, char c, tui_key_sink send, void *ctx) {
    const char end[] = "\033[201~";
    if (c == end[input->sequence_len]) {
        if (++input->sequence_len == sizeof(end) - 1) {
            if (input->paste_overflow)
                send(ctx, TUI_PASTE, NULL, 0);
            else if (input->paste_len)
                send(ctx, TUI_PASTE, input->pasted, input->paste_len);
            volatile char *bytes = input->pasted;
            for (size_t i = 0; i < input->paste_len; ++i)
                bytes[i] = 0;
            input->paste = false;
            input->paste_overflow = false;
            input->paste_len = input->sequence_len = 0;
        }
        return;
    }
    for (size_t i = 0; i < input->sequence_len; ++i)
        paste_append(input, end[i]);
    input->sequence_len = c == '\033' ? 1 : 0;
    if (!input->sequence_len)
        paste_append(input, c);
}

static bool in_control(const tui_input *input) {
    return input->sequence_len >= 2 &&
           (input->sequence[1] == ']' || input->sequence[1] == '_');
}

static void control_byte(tui_input *input, char c) {
    bool end = (input->control_escape && c == '\\') ||
               (input->sequence[1] == ']' && c == '\a');
    if (end) {
        if (!input->control_overflow) {
            size_t length = input->sequence_len;
            if (input->control_escape && c == '\\')
                --length;
            input->sequence[length] = 0;
            tui_graphics_reply(input->graphics, input->sequence);
        }
        input->sequence_len = 0;
        input->control_overflow = input->control_escape = false;
        return;
    }
    input->control_escape = c == '\033';
    if (input->sequence_len == sizeof(input->sequence) - 1 || c == 0)
        input->control_overflow = true;
    if (!input->control_overflow)
        input->sequence[input->sequence_len++] = c;
}

static void escape_byte(tui_input *input, char c, tui_key_sink send, void *ctx) {
    if (input->sequence_len >= sizeof(input->sequence) - 1) {
        input->sequence_len = 0;
        return;
    }
    input->sequence[input->sequence_len++] = c;
    if (input->sequence_len == 2 && (c == '[' || c == 'O' || c == ']' || c == '_')) {
        input->control_overflow = input->control_escape = false;
        return;
    }
    if ((unsigned char)c < 0x40)
        return;
    input->sequence[input->sequence_len] = 0;
    const char *code = input->sequence + 1;
    size_t length = input->sequence_len;
    input->sequence_len = 0;
    if (length < 3 || (*code != '[' && *code != 'O'))
        return;
    tui_key key;
    if (!strcmp(code, "[200~")) {
        input->paste = true;
        return;
    }
    tui_graphics_reply(input->graphics, input->sequence);
    switch (c) {
    case 'A': key = TUI_UP; break;
    case 'B': key = TUI_DOWN; break;
    case 'C': key = TUI_RIGHT; break;
    case 'D': key = TUI_LEFT; break;
    case 'H': key = TUI_HOME; break;
    case 'F': key = TUI_END; break;
    case 'Z':
        if (strcmp(code, "[Z"))
            return;
        key = TUI_SHIFT_TAB;
        break;
    case '~':
        if (!strcmp(code, "[3~"))
            key = TUI_DELETE;
        else if (!strcmp(code, "[1~") || !strcmp(code, "[7~"))
            key = TUI_HOME;
        else if (!strcmp(code, "[4~") || !strcmp(code, "[8~"))
            key = TUI_END;
        else
            return;
        break;
    default: return;
    }
    send(ctx, key, NULL, 0);
}

static void utf8_byte(tui_input *input, unsigned char c, tui_key_sink send, void *ctx) {
    if (!input->utf8_len) {
        input->utf8_need = c >= 0xf0 && c <= 0xf4 ? 4 : c >= 0xe0 && c <= 0xef ? 3
                         : c >= 0xc2 && c <= 0xdf ? 2 : 0;
        if (!input->utf8_need)
            return;
    }
    input->utf8[input->utf8_len++] = (char)c;
    if (input->utf8_len == input->utf8_need) {
        utf8proc_int32_t cp;
        if (utf8proc_iterate((const unsigned char *)input->utf8,
                             (utf8proc_ssize_t)input->utf8_len, &cp) > 0 && cp >= 160)
            send(ctx, TUI_TEXT, input->utf8, input->utf8_len);
        input->utf8_len = 0;
    }
}

void tui_input_read(tui_input *input, const char *bytes, size_t count, uint64_t now,
                    tui_key_sink send, void *ctx) {
    for (size_t i = 0; i < count; ++i) {
        unsigned char c = (unsigned char)bytes[i];
        if (input->paste) {
            paste_byte(input, (char)c, send, ctx);
            continue;
        }
        if (in_control(input)) {
            input->escape_at = now;
            control_byte(input, (char)c);
            continue;
        }
        if (input->sequence_len && c != 27) {
            escape_byte(input, (char)c, send, ctx);
            continue;
        }
        if (c >= 128) {
            utf8_byte(input, c, send, ctx);
            continue;
        }
        input->utf8_len = 0;
        tui_key key;
        switch (c) {
        case 1: key = TUI_HOME; break;
        case 3: key = TUI_INTERRUPT; break;
        case 4: key = TUI_EOF; break;
        case 5: key = TUI_END; break;
        case 8: case 127: key = TUI_BACKSPACE; break;
        case 9: key = TUI_TAB; break;
        case 10: key = TUI_NEWLINE; break;
        case 13: key = TUI_ENTER; break;
        case 18: key = TUI_REVEAL; break;
        case 19: key = TUI_SAVE; break;
        case 27:
            if (input->sequence_len == 1)
                send(ctx, TUI_ESCAPE, NULL, 0);
            input->sequence[0] = 27;
            input->sequence_len = 1;
            input->escape_at = now;
            continue;
        default:
            if (c >= 32)
                send(ctx, TUI_TEXT, bytes + i, 1);
            continue;
        }
        send(ctx, key, NULL, 0);
    }
}

int tui_input_timeout(const tui_input *input, uint64_t now) {
    if (!input->sequence_len || input->paste)
        return -1;
    uint64_t elapsed = now - input->escape_at;
    unsigned limit = in_control(input) ? 120 : 40;
    return elapsed < limit ? (int)(limit - elapsed) : 0;
}

void tui_input_expire(tui_input *input, uint64_t now, tui_key_sink send, void *ctx) {
    if (!tui_input_timeout(input, now)) {
        bool lone_escape = input->sequence_len == 1;
        input->sequence_len = 0;
        input->control_overflow = input->control_escape = false;
        if (lone_escape)
            send(ctx, TUI_ESCAPE, NULL, 0);
    }
}
