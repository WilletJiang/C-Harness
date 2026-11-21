#include "content_internal.h"
#include <ctype.h>
#include <stdio.h>

bool content_image_mime(const char *mime) {
    return mime && (!strcmp(mime, "image/png") || !strcmp(mime, "image/jpeg") ||
                    !strcmp(mime, "image/gif") || !strcmp(mime, "image/webp"));
}
bool content_id_valid(const char *id) {
    if (!id || strlen(id) != 64)
        return false;
    for (size_t i = 0; i < 64; i++)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f')))
            return false;
    return true;
}
const char *content_filename_mime(const char *name) {
    const char *dot = name ? strrchr(name, '.') : NULL;
    if (!dot)
        return NULL;
    if (!strcmp(dot, ".pdf"))
        return "application/pdf";
    if (!strcmp(dot, ".txt"))
        return "text/plain";
    if (!strcmp(dot, ".json"))
        return "application/json";
    if (!strcmp(dot, ".png"))
        return "image/png";
    if (!strcmp(dot, ".jpg") || !strcmp(dot, ".jpeg"))
        return "image/jpeg";
    if (!strcmp(dot, ".gif"))
        return "image/gif";
    if (!strcmp(dot, ".webp"))
        return "image/webp";
    return NULL;
}
bool content_mime_valid(const char *mime, const unsigned char *p, size_t n) {
    if (!mime || !p || !n || n > AH_ARTIFACT_MAX_BYTES)
        return false;
    if (!strcmp(mime, "image/png"))
        return n >= 45 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8) && !memcmp(p + 12, "IHDR", 4) &&
               !memcmp(p + n - 8, "IEND\xae\x42\x60\x82", 8);
    if (!strcmp(mime, "image/jpeg"))
        return n >= 5 && p[0] == 0xff && p[1] == 0xd8 && p[2] == 0xff && p[n - 2] == 0xff &&
               p[n - 1] == 0xd9;
    if (!strcmp(mime, "image/gif"))
        return n >= 14 && (!memcmp(p, "GIF87a", 6) || !memcmp(p, "GIF89a", 6)) && p[n - 1] == 0x3b;
    if (!strcmp(mime, "image/webp")) {
        uint32_t size = n >= 12 ? (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 |
                                      (uint32_t)p[7] << 24
                                : 0;
        return n >= 20 && !memcmp(p, "RIFF", 4) && !memcmp(p + 8, "WEBP", 4) &&
               (size_t)size + 8 == n;
    }
    if (!strcmp(mime, "application/pdf")) {
        while (n && isspace(p[n - 1]))
            n--;
        return n >= 13 && !memcmp(p, "%PDF-", 5) && !memcmp(p + n - 5, "%%EOF", 5);
    }
    if (strcmp(mime, "text/plain") && strcmp(mime, "application/json"))
        return false;
    size_t complete = 0;
    if (memchr(p, 0, n) || !ah_utf8_prefix((const char *)p, n, &complete) || complete != n)
        return false;
    if (!strcmp(mime, "text/plain"))
        return true;
    yyjson_doc *doc = yyjson_read((const char *)p, n, 0);
    bool valid = doc && ah_json_check(yyjson_doc_get_root(doc), NULL) == AH_OK;
    yyjson_doc_free(doc);
    return valid;
}
void content_blob_id(content_blob *blob) {
    ah_hash hash;
    ah_hash_init(&hash);
    ah_hash_update(&hash, blob->mime, strlen(blob->mime) + 1);
    ah_hash_update(&hash, blob->data, blob->size);
    ah_hash_finish(&hash, blob->id);
}
static int base64_digit(unsigned char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}
ah_code content_decode(const char *encoded, const char *fallback_mime, content_blob **out,
                       ah_error *err) {
    *out = NULL;
    const char *mime = fallback_mime, *data = encoded;
    char explicit_mime[64];
    if (!strncmp(encoded, "data:", 5)) {
        const char *separator = strstr(encoded + 5, ";base64,");
        size_t n = separator ? (size_t)(separator - encoded - 5) : 0;
        if (!n || n >= sizeof(explicit_mime))
            return ah_fail(err, AH_INVALID, "expected MIME base64 data URL");
        memcpy(explicit_mime, encoded + 5, n);
        explicit_mime[n] = 0;
        mime = explicit_mime;
        data = separator + 8;
    }
    size_t n = strlen(data);
    if (!mime || !n || n % 4)
        return ah_fail(err, AH_INVALID, "padded base64 and explicit supported MIME required");
    size_t padding = (data[n - 1] == '=') + (data[n - 2] == '=');
    size_t size = n / 4 * 3 - padding;
    if (!size || size > AH_ARTIFACT_MAX_BYTES)
        return ah_fail(err, AH_LIMIT, "artifact byte limit exceeded");
    content_blob *blob = calloc(1, sizeof(*blob));
    if (!blob)
        return ah_fail(err, AH_IO, "out of memory");
    blob->data = malloc(size);
    blob->mime = ah_strdup(mime);
    blob->size = size;
    ah_code rc = AH_OK;
    if (!blob->data || !blob->mime)
        rc = ah_fail(err, AH_IO, "out of memory");
    size_t written = 0;
    for (size_t i = 0; rc == AH_OK && i < n; i += 4) {
        int a = base64_digit(data[i]), b = base64_digit(data[i + 1]);
        int c = data[i + 2] == '=' ? 0 : base64_digit(data[i + 2]);
        int d = data[i + 3] == '=' ? 0 : base64_digit(data[i + 3]);
        bool last = i + 4 == n;
        if (a < 0 || b < 0 || c < 0 || d < 0 ||
            ((!last || padding == 0) && (data[i + 2] == '=' || data[i + 3] == '=')) ||
            (data[i + 2] == '=' && data[i + 3] != '=') || (last && padding == 2 && (b & 15)) ||
            (last && padding == 1 && (c & 3))) {
            rc = ah_fail(err, AH_INVALID, "invalid or noncanonical base64");
            break;
        }
        unsigned bits = (unsigned)a << 18 | (unsigned)b << 12 | (unsigned)c << 6 | (unsigned)d;
        blob->data[written++] = (unsigned char)(bits >> 16);
        if (written < size)
            blob->data[written++] = (unsigned char)(bits >> 8);
        if (written < size)
            blob->data[written++] = (unsigned char)bits;
    }
    if (rc == AH_OK && !content_mime_valid(mime, blob->data, size))
        rc = ah_fail(err, AH_INVALID, "artifact bytes do not match supported MIME");
    if (rc == AH_OK) {
        content_blob_id(blob);
        *out = blob;
    } else {
        free(blob->data);
        free(blob->mime);
        free(blob);
    }
    return rc;
}
char *content_data_url(const char *mime, const unsigned char *data, size_t size) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t prefix = strlen(mime) + 13, encoded = (size + 2) / 3 * 4;
    char *out = malloc(prefix + encoded + 1);
    if (!out)
        return NULL;
    snprintf(out, prefix + 1, "data:%s;base64,", mime);
    size_t at = prefix;
    for (size_t i = 0; i < size; i += 3) {
        unsigned v = (unsigned)data[i] << 16;
        if (i + 1 < size)
            v |= (unsigned)data[i + 1] << 8;
        if (i + 2 < size)
            v |= data[i + 2];
        out[at++] = alphabet[v >> 18];
        out[at++] = alphabet[(v >> 12) & 63];
        out[at++] = i + 1 < size ? alphabet[(v >> 6) & 63] : '=';
        out[at++] = i + 2 < size ? alphabet[v & 63] : '=';
    }
    out[at] = 0;
    return out;
}
