#ifndef AH_CONTENT_H
#define AH_CONTENT_H
#include "ah.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AH_ARTIFACT_ID_CAP 65
#define AH_ARTIFACT_MAX_BYTES (8u * 1024u * 1024u)
#define AH_CONTENT_MAX_PARTS 64

/* A bounded Responses content array: input_text, input_image, input_file.
 * Images accept image_url (HTTP(S) or a base64 data URL), file_id, or artifact_id.
 * Files accept file_url, file_id, file_data, or artifact_id. Local/raw data
 * requires filename. Unknown fields, duplicate keys and ambiguous sources fail.
 * Inline binary data is stored once; journal/commands contain immutable references.
 * Only explicitly submitted artifacts are expanded in model requests. */
ah_code ah_submit_content(ah_engine *, const char *session_id, const char *command_id,
                          const char *content_json, ah_error *);

/* Owner-thread import, regular files only, final symlinks rejected. Supported MIME:
 * image/png, image/jpeg, image/gif, image/webp, application/pdf, text/plain,
 * application/json. Signatures/UTF-8 are checked, not full document validity.
 * ID hashes MIME + NUL + bytes; identical imports deduplicate. Never sends data.
 * References belong to the importing session and are inherited by its forks. */
ah_code ah_artifact_import(ah_engine *, const char *session_id, const char *path, const char *mime,
                           char id[AH_ARTIFACT_ID_CAP], ah_error *);
ah_code ah_artifacts_json(ah_engine *, const char *session_id, char **json, ah_error *);

/* Explicit permanent deletion, not compaction or archive. Requires idle/completed
 * or stopped/failed sessions, settled operations, no uncertain verification and
 * no pending messages. Task acceptance is not required. An agent tree must
 * be deleted as a whole from its root. Surviving shared receipt references block
 * deletion; forget dependent forks first. Shared artifact bytes are preserved.
 * Deletes the selected audit history and reclaims unreferenced artifact rows. */
ah_code ah_forget(ah_engine *, const char *session_id, bool include_agent_tree, ah_error *);
/* Explicit physical SQLite space reclamation. Does not delete additional history. */
ah_code ah_storage_vacuum(ah_engine *, ah_error *);

#ifdef __cplusplus
}
#endif
#endif
