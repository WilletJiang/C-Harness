#include "tui_graphics.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

enum { MAX_PNG = 128 * 1024, CHUNK_BYTES = 3072, MAX_CELL_PIXELS = 128 };
#define PROBE_ID "3670401489"

extern size_t ah_tui_math_png(const char *, size_t, unsigned em, unsigned width, unsigned height,
                              uint32_t rgb, unsigned char *, size_t capacity);

static void dimensions(tui_graphics *g) {
    struct winsize size = {0};
    g->cell_width = g->cell_height = 0;
    if (!ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) && size.ws_col && size.ws_row) {
        unsigned x = size.ws_xpixel / size.ws_col;
        unsigned y = size.ws_ypixel / size.ws_row;
        if (x && y && x <= MAX_CELL_PIXELS && y <= MAX_CELL_PIXELS) {
            g->cell_width = x;
            g->cell_height = y;
            return;
        }
    }
    fputs("\033[16t", stdout);
    fflush(stdout);
}

static bool ready(const tui_graphics *g) {
    return g && g->kitty && g->cell_width && g->cell_height &&
           (g->fixed_ink || (g->have_foreground && g->have_background));
}

static void query_colors(tui_graphics *g) {
    if (g->fixed_ink)
        return;
    fputs("\033]10;?\033\\\033]11;?\033\\", stdout);
    fflush(stdout);
}

void tui_graphics_start(tui_graphics *g, unsigned cols, unsigned rows, uint64_t now) {
    const char *mode = getenv("AH_TUI_MATH");
    const char *term = getenv("TERM");
    *g = (tui_graphics){.cols = cols, .rows = rows};
    if (getenv("TMUX") || (mode && !strcmp(mode, "text")) || !term ||
        !strncmp(term, "screen", 6) || !strncmp(term, "tmux", 4))
        return;
    if (mode && (!strcmp(mode, "light") || !strcmp(mode, "dark"))) {
        g->fixed_ink = true;
        g->foreground = !strcmp(mode, "dark") ? 0xe8ecf4 : 0x202328;
    }
    g->probing = true;
    g->probe_until = now + 120;
    fputs("\033_Gi=" PROBE_ID ",a=q,t=d,f=24,s=1,v=1;AAAA\033\\", stdout);
    dimensions(g);
    query_colors(g);
    fflush(stdout);
}

void tui_graphics_resize(tui_graphics *g, unsigned cols, unsigned rows) {
    if (g->cols == cols && g->rows == rows)
        return;
    g->cols = cols;
    g->rows = rows;
    if (g->kitty)
        dimensions(g);
}

void tui_graphics_colors(tui_graphics *g) {
    if (g->kitty)
        query_colors(g);
}

int tui_graphics_wait(const tui_graphics *g, uint64_t now) {
    return g->probing && !ready(g) && now < g->probe_until ? (int)(g->probe_until - now) : -1;
}

static int hex_digit(char ch) {
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

static bool rgb_value(const char *s, uint32_t *rgb) {
    if (strncmp(s, "rgb:", 4))
        return false;
    s += 4;
    uint32_t value = 0;
    for (unsigned i = 0; i < 3; ++i) {
        unsigned component = 0, digits = 0;
        for (int x = hex_digit(*s); x >= 0; x = hex_digit(*s)) {
            if (++digits > 4)
                return false;
            component = 16 * component + (unsigned)x;
            ++s;
        }
        if (!digits || *s != (i < 2 ? '/' : 0))
            return false;
        unsigned max = (1u << (4 * digits)) - 1;
        value = (value << 8) | ((component * 255 + max / 2) / max);
        if (i < 2)
            ++s;
    }
    *rgb = value;
    return true;
}

static unsigned cell_pixels(const char **value, char delimiter) {
    const char *p = *value;
    unsigned n = 0;
    while (*p >= '0' && *p <= '9' && n <= MAX_CELL_PIXELS) {
        n = 10 * n + (unsigned)(*p - '0');
        ++p;
    }
    if (!n || n > MAX_CELL_PIXELS || *p != delimiter)
        return 0;
    *value = p + 1;
    return n;
}

void tui_graphics_reply(tui_graphics *g, const char *sequence) {
    if (!g || !g->probing)
        return;
    if (!strcmp(sequence, "\033_Gi=" PROBE_ID ";OK")) {
        g->kitty = true;
    } else if (!strncmp(sequence, "\033]10;", 5) && !g->fixed_ink) {
        g->have_foreground = rgb_value(sequence + 5, &g->foreground);
    } else if (!strncmp(sequence, "\033]11;", 5) && !g->fixed_ink) {
        g->have_background = rgb_value(sequence + 5, &g->background);
    } else if (!strncmp(sequence, "\033[6;", 4)) {
        const char *p = sequence + 4;
        unsigned y = cell_pixels(&p, ';');
        unsigned x = y ? cell_pixels(&p, 't') : 0;
        if (x && !*p) {
            g->cell_width = x;
            g->cell_height = y;
        }
    }
}

static double linear(unsigned channel) {
    double s = channel / 255.0;
    return s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4);
}

static double luminance(uint32_t rgb) {
    return .2126 * linear((rgb >> 16) & 255) + .7152 * linear((rgb >> 8) & 255) +
           .0722 * linear(rgb & 255);
}

static uint32_t ink(const tui_graphics *g) {
    if (g->fixed_ink)
        return g->foreground;
    double bg = luminance(g->background) + .05, fg = luminance(g->foreground) + .05;
    if (fmax(bg, fg) / fmin(bg, fg) >= 4.5)
        return g->foreground;
    return bg / .05 >= 1.05 / bg ? 0 : 0xffffff;
}

static unsigned be32(const unsigned char *p) {
    return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
}

bool tui_graphics_render(const tui_graphics *g, const char *source, size_t size,
                         unsigned gutter, unsigned available, tui_graphic *image) {
    if (!ready(g) || !available || g->rows < 4 || size > 4096)
        return false;
    unsigned max_width = available * g->cell_width;
    unsigned max_height = (g->rows - 2) * g->cell_height;
    unsigned char *png = malloc(MAX_PNG);
    if (!png)
        return false;
    unsigned em = (4 * g->cell_height + 2) / 5;
    size_t bytes = ah_tui_math_png(source, size, em, max_width, max_height, ink(g), png, MAX_PNG);
    unsigned width = bytes >= 24 ? be32(png + 16) : 0;
    unsigned height = bytes >= 24 ? be32(png + 20) : 0;
    if (!width || !height || width > max_width || height > max_height) {
        free(png);
        return false;
    }
    unsigned left = (max_width - width) / 2;
    *image = (tui_graphic){.png = png, .size = bytes,
                           .rows = (height + g->cell_height - 1) / g->cell_height,
                           .left = gutter + left / g->cell_width,
                           .offset_x = left % g->cell_width};
    return true;
}

static void encode(FILE *out, const unsigned char *p, size_t size) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char encoded[4096];
    size_t used = 0;
    for (size_t i = 0; i < size; i += 3) {
        unsigned a = p[i], b = i + 1 < size ? p[i + 1] : 0, c = i + 2 < size ? p[i + 2] : 0;
        encoded[used++] = alphabet[a >> 2];
        encoded[used++] = alphabet[((a & 3) << 4) | (b >> 4)];
        encoded[used++] = i + 1 < size ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        encoded[used++] = i + 2 < size ? alphabet[c & 63] : '=';
    }
    fwrite(encoded, 1, used, out);
}

void tui_graphics_place(FILE *out, tui_graphic *image, const char *gutter) {
    /* Reserve physical lines first so placement never runs below the viewport. */
    for (unsigned i = 0; i < image->rows; ++i) {
        fputs(gutter, out);
        fputs("\r\n", out);
    }
    fprintf(out, "\033[%uA\r", image->rows);
    if (image->left)
        fprintf(out, "\033[%uC", image->left);
    for (size_t at = 0; at < image->size;) {
        size_t count = image->size - at;
        if (count > CHUNK_BYTES)
            count = CHUNK_BYTES;
        if (!at)
            fprintf(out, "\033_Ga=T,t=d,f=100,C=1,X=%u,q=2,", image->offset_x);
        else
            fputs("\033_G", out);
        fprintf(out, "m=%u;", (unsigned)(at + count < image->size));
        encode(out, image->png + at, count);
        fputs("\033\\", out);
        at += count;
    }
    fprintf(out, "\r\033[%uB", image->rows);
    free(image->png);
}
