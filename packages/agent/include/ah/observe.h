#ifndef AH_OBSERVE_H
#define AH_OBSERVE_H
#include "ah.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ah_reader ah_reader;

/* Independent read-only connection to a runtime-managed schema-8 file using
 * DELETE journaling. Opening does not create the database file, run harness
 * recovery or migrations, or acquire the engine's writer ownership lock.
 * One thread owns each reader; it may observe a writer on another thread/process. */
ah_code ah_reader_open(const char *database_path, ah_reader **, ah_error *);
/* SQL reads on a separate connection while an engine in this process owns the
 * database. On macOS this requires a writable file for SQLite lock compatibility. */
ah_code ah_reader_open_with_writer(const char *database_path, ah_reader **, ah_error *);
void ah_reader_close(ah_reader *);
/* Defaults to 1000 ms. A zero timeout makes lock contention return immediately. */
ah_code ah_reader_set_busy_timeout(ah_reader *, unsigned milliseconds, ah_error *);
/* Consistent session snapshot plus "cursor", the last global event sequence in
 * that same read transaction. Read events after cursor to continue without gaps. */
ah_code ah_reader_session_json(ah_reader *, const char *session_id, char **json, ah_error *);
/* Copies at most limit (1..256) durable events and 4 MiB before invoking callbacks,
 * releasing SQLite read locks first. NULL session reads across all sessions.
 * last_seq starts at after_seq and advances only for delivered events. A first
 * event larger than 4 MiB returns AH_LIMIT without callbacks or cursor advance;
 * a later oversized event ends the current batch. Retry with the returned cursor.
 * Observer values are borrowed; do not close or reenter this reader in callbacks. */
ah_code ah_reader_events(ah_reader *, const char *session_id, uint64_t after_seq, unsigned limit,
                         ah_observer, void *context, uint64_t *last_seq, ah_error *);
/* Reads the newest bounded page preceding before_seq (zero uses the newest
 * event). Delivers the page in chronological order. first_seq is zero when
 * the page is empty. The count, byte and callback limits match ah_reader_events. */
ah_code ah_reader_recent_events(ah_reader *, const char *session_id, uint64_t before_seq,
                                unsigned limit, ah_observer, void *context,
                                uint64_t *first_seq, ah_error *);

#ifdef __cplusplus
}
#endif
#endif
