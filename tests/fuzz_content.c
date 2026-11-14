#include "core/content_internal.h"
#include <assert.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 65536)
        return 0;
    char *input = malloc(size + 1);
    if (!input)
        return 0;
    memcpy(input, data, size);
    input[size] = 0;
    content_blob *blob = NULL;
    ah_error error = {0};
    if (content_decode(input, "text/plain", &blob, &error) == AH_OK) {
        assert(blob && blob->size && blob->size <= AH_ARTIFACT_MAX_BYTES);
        char *url = content_data_url(blob->mime, blob->data, blob->size);
        assert(url);
        content_blob *roundtrip = NULL;
        assert(content_decode(url, NULL, &roundtrip, &error) == AH_OK);
        assert(roundtrip->size == blob->size && !strcmp(roundtrip->id, blob->id));
        assert(!memcmp(roundtrip->data, blob->data, blob->size));
        free(roundtrip->data);
        free(roundtrip->mime);
        free(roundtrip);
        free(url);
        free(blob->data);
        free(blob->mime);
        free(blob);
    } else
        assert(!blob);
    free(input);
    return 0;
}
