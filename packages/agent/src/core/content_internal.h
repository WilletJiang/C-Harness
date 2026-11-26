#ifndef AH_CONTENT_INTERNAL_H
#define AH_CONTENT_INTERNAL_H
#include "core.h"
#include "ah/content.h"

typedef struct content_blob {
    char id[AH_ARTIFACT_ID_CAP];
    char *mime;
    unsigned char *data;
    size_t size;
    struct content_blob *next;
} content_blob;
typedef struct {
    char *json;
    content_blob *blobs;
} core_content;

ah_code core_content_prepare(ah_engine *, const char *, const char *, core_content *, ah_error *);
ah_code core_content_parts_prepare(ah_engine *, const char *, const char *, size_t, core_content *,
                                   ah_error *);
void core_content_free(core_content *);
ah_code core_content_save(ah_engine *, const char *, const core_content *, ah_error *);
ah_code core_submit_item(ah_engine *, const char *, const char *, const char *, int, bool,
                         const core_content *, ah_error *);
ah_code core_submit_first(ah_engine *, const char *model, const char *instructions,
                          const char *effort, const char *command, const char *input,
                          const content_blob *image, char id[AH_ID_CAP], uint64_t *cursor,
                          ah_error *);
bool content_mime_valid(const char *, const unsigned char *, size_t);
bool content_image_mime(const char *);
bool content_id_valid(const char *);
const char *content_filename_mime(const char *);
void content_blob_id(content_blob *);
ah_code content_decode(const char *, const char *, content_blob **, ah_error *);
char *content_data_url(const char *, const unsigned char *, size_t);
ah_code content_blob_save(ah_engine *, const char *, const content_blob *, ah_error *);
ah_code content_artifact_read(const char *path, const char *mime, content_blob *, ah_error *);
ah_code content_artifact_save(ah_engine *, const char *, const content_blob *, ah_error *);
ah_code content_reference(ah_engine *, const char *, const char *, bool, ah_error *);

#endif
