#ifndef AH_RESPONSES_WS_IO_H
#define AH_RESPONSES_WS_IO_H
#include "responses_internal.h"

typedef struct responses_ws_io responses_ws_io;
enum { AH_WS_IO_CHUNK = 16384 };

/* The owner performs the upgrade. After registration only the pump calls
 * curl on the socket, except while the owner has paused it for teardown. */
responses_ws_io *responses_ws_io_open(ah_responses *, CURL *, curl_socket_t);
void responses_ws_io_close(responses_ws_io *);
void responses_ws_io_shutdown(ah_responses *);
size_t responses_ws_io_pause(responses_ws_io *, bool *submitted);
void responses_ws_io_resume(responses_ws_io *, uint64_t idle_until);
CURLcode responses_ws_io_status(responses_ws_io *);
void responses_ws_io_probe(responses_ws_io *);
CURLcode responses_ws_io_probe_result(responses_ws_io *);
CURLcode responses_ws_io_send(responses_ws_io *, const char *, size_t, size_t *, bool *);
CURLcode responses_ws_io_recv(responses_ws_io *, char *, size_t *, struct curl_ws_frame *);
#endif
