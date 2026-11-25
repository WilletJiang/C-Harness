#include "responses_ws_io.h"

#ifdef AH_HAVE_CURL_WS
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>

enum { WRITE_FRAGMENT = 65536, INBOX_FRAMES = 64, CONTROL_BYTES = 125 };

typedef struct ws_packet {
    struct ws_packet *next;
    struct curl_ws_frame meta;
    char bytes[];
} ws_packet;

typedef struct {
    const char *bytes;
    size_t size, written, reported, frame_end;
    bool started, done;
} ws_write;

typedef enum { PROBE_IDLE, PROBE_QUEUED, PROBE_WRITING, PROBE_WAITING, PROBE_ACKED } ws_probe;

struct responses_ws_io {
    struct responses_ws_io *next;
    struct responses_ws_io_pool *pool;
    CURL *easy;
    curl_socket_t socket;
    ws_write write;
    ws_packet *head, *tail;
    unsigned queued;
    uint64_t idle_until;
    CURLcode failure;
    unsigned ctl_type;
    size_t ctl_size, ctl_sent;
    char control[CONTROL_BYTES];
    ws_probe probe;
    uint64_t probe_nonce;
    size_t probe_sent;
    bool probe_match;
    bool paused, read_buffered;
#ifdef AH_WS_IO_TEST
    const char *release_steer_completion;
    bool block_after_steer_ack, delivered_steer_ack;
#endif
};

struct responses_ws_io_pool {
    pthread_mutex_t lock;
    pthread_t thread;
    CURLM *owner;
    responses_ws_io *links;
    int wake[2];
    uint64_t revision;
    bool stopping;
};

static void wake_pump(struct responses_ws_io_pool *pool) {
    ssize_t wrote;
    do {
        wrote = write(pool->wake[1], "", 1);
    } while (wrote < 0 && errno == EINTR);
}

static void wake_owner(responses_ws_io *io) {
    if (!io->idle_until)
        (void)curl_multi_wakeup(io->pool->owner);
}

static void failed(responses_ws_io *io, CURLcode code) {
    io->failure = code;
    wake_owner(io);
}

static bool enqueue(responses_ws_io *io, const struct curl_ws_frame *meta, const char *bytes,
                    size_t length) {
    ws_packet *item = malloc(sizeof(*item) + length);
    if (!item)
        return false;
    item->next = NULL;
    item->meta = *meta;
    item->meta.len = length;
    memcpy(item->bytes, bytes, length);
    if (io->tail)
        io->tail->next = item;
    else
        io->head = item;
    io->tail = item;
    ++io->queued;
    wake_owner(io);
    return true;
}

static void receive_pong(responses_ws_io *io, const struct curl_ws_frame *meta,
                         const char *bytes, size_t length) {
    if (io->probe != PROBE_WAITING)
        return;
    uint64_t offset = (uint64_t)meta->offset;
    if (!offset)
        io->probe_match = true;
    if (offset > sizeof(io->probe_nonce) || length > sizeof(io->probe_nonce) - offset ||
        memcmp((const char *)&io->probe_nonce + offset, bytes, length))
        io->probe_match = false;
    if (io->probe_match && !meta->bytesleft && offset + length == sizeof(io->probe_nonce)) {
        io->probe = PROBE_ACKED;
        wake_owner(io);
    }
}

static void read_packet(responses_ws_io *io) {
    char bytes[AH_WS_IO_CHUNK];
    size_t length = 0;
    const struct curl_ws_frame *meta = NULL;
    CURLcode code = curl_ws_recv(io->easy, bytes, sizeof(bytes), &length, &meta);
    io->read_buffered = code == CURLE_OK;
    if (code == CURLE_AGAIN)
        return;
    if (code != CURLE_OK || !meta) {
        failed(io, code != CURLE_OK ? code : CURLE_WEIRD_SERVER_REPLY);
        return;
    }
    if (meta->flags & CURLWS_PONG) {
        receive_pong(io, meta, bytes, length);
        return;
    }
    if (meta->flags & (CURLWS_PING | CURLWS_CLOSE)) {
        if (meta->offset == 0)
            io->ctl_size = 0;
        if (meta->offset != (curl_off_t)io->ctl_size || length > sizeof(io->control) - io->ctl_size) {
            failed(io, CURLE_WEIRD_SERVER_REPLY);
            return;
        }
        memcpy(io->control + io->ctl_size, bytes, length);
        io->ctl_size += length;
        if (meta->bytesleft)
            return;
        io->ctl_type = meta->flags & CURLWS_CLOSE ? CURLWS_CLOSE : CURLWS_PONG;
        if (io->ctl_type == CURLWS_PONG)
            return;
        struct curl_ws_frame close = *meta;
        close.offset = 0;
        close.len = io->ctl_size;
        if (!enqueue(io, &close, io->control, io->ctl_size))
            failed(io, CURLE_OUT_OF_MEMORY);
        return;
    }
    if (!enqueue(io, meta, bytes, length))
        failed(io, CURLE_OUT_OF_MEMORY);
}

static bool writing(const responses_ws_io *io) {
    return io->ctl_type || io->probe == PROBE_QUEUED || io->probe == PROBE_WRITING ||
           (io->write.bytes && !io->write.done);
}

static void write_probe(responses_ws_io *io) {
    io->probe = PROBE_WRITING;
    size_t sent = 0;
    CURLcode code = curl_ws_send(io->easy, (const char *)&io->probe_nonce + io->probe_sent,
                                 sizeof(io->probe_nonce) - io->probe_sent, &sent, 0, CURLWS_PING);
    io->probe_sent += sent;
    if (code != CURLE_OK && code != CURLE_AGAIN)
        failed(io, code);
    else if (code == CURLE_OK && io->probe_sent == sizeof(io->probe_nonce))
        io->probe = PROBE_WAITING;
}

static void write_packet(responses_ws_io *io) {
    ws_write *tx = &io->write;
    size_t sent = 0;
    if (io->probe == PROBE_WRITING || (io->probe == PROBE_QUEUED && !io->ctl_type)) {
        write_probe(io);
        return;
    }
    if (io->ctl_type && !tx->frame_end) {
        CURLcode code = curl_ws_send(io->easy, io->control + io->ctl_sent,
                                     io->ctl_size - io->ctl_sent, &sent, 0, io->ctl_type);
        io->ctl_sent += sent;
        if (code != CURLE_OK && code != CURLE_AGAIN)
            failed(io, code);
        else if (code == CURLE_OK && io->ctl_sent == io->ctl_size) {
            if (io->ctl_type == CURLWS_CLOSE)
                failed(io, CURLE_GOT_NOTHING);
            io->ctl_type = 0;
            io->ctl_size = io->ctl_sent = 0;
        }
        return;
    }
    if (!tx->frame_end) {
        size_t remaining = tx->size - tx->written;
        tx->frame_end = tx->written + (remaining > WRITE_FRAGMENT ? WRITE_FRAGMENT : remaining);
    }
    unsigned flags = CURLWS_TEXT | (tx->frame_end < tx->size ? CURLWS_CONT : 0);
    tx->started = true;
    CURLcode code = curl_ws_send(io->easy, tx->bytes + tx->written,
                                 tx->frame_end - tx->written, &sent, 0, flags);
    tx->written += sent;
    if (code != CURLE_OK && code != CURLE_AGAIN)
        failed(io, code);
    else if (code == CURLE_OK && tx->written == tx->frame_end) {
        tx->frame_end = 0;
        tx->done = tx->written == tx->size;
        if (tx->done)
            wake_owner(io);
    }
}

static bool reading(const responses_ws_io *io) {
    return !io->ctl_type && io->queued < INBOX_FRAMES;
}

static void drain_wake(int fd) {
    char bytes[128];
    ssize_t count;
    do {
        count = read(fd, bytes, sizeof(bytes));
    } while (count > 0 || (count < 0 && errno == EINTR));
}

static void *pump_main(void *context) {
    struct responses_ws_io_pool *pool = context;
    struct pollfd *fds = NULL;
    responses_ws_io **links = NULL;
    size_t capacity = 0;
    pthread_mutex_lock(&pool->lock);
    while (!pool->stopping) {
        size_t needed = 1;
        for (responses_ws_io *io = pool->links; io; io = io->next)
            ++needed;
        if (needed > capacity) {
            struct pollfd *new_fds = malloc(needed * sizeof(*fds));
            responses_ws_io **new_links = malloc(needed * sizeof(*links));
            if (!new_fds || !new_links) {
                free(new_fds);
                free(new_links);
                for (responses_ws_io *io = pool->links; io; io = io->next)
                    if (!io->failure)
                        failed(io, CURLE_OUT_OF_MEMORY);
                pthread_mutex_unlock(&pool->lock);
                struct pollfd wake = {.fd = pool->wake[0], .events = POLLIN};
                if (poll(&wake, 1, -1) > 0)
                    drain_wake(pool->wake[0]);
                pthread_mutex_lock(&pool->lock);
                continue;
            }
            free(fds);
            free(links);
            fds = new_fds;
            links = new_links;
            capacity = needed;
        }
        fds[0] = (struct pollfd){.fd = pool->wake[0], .events = POLLIN};
        uint64_t now = ah_now_ms(), revision = pool->revision;
        int timeout = -1;
        size_t count = 1;
        for (responses_ws_io *io = pool->links; io; io = io->next) {
            if (io->paused || io->failure)
                continue;
            if (io->idle_until) {
                if (now >= io->idle_until) {
                    failed(io, CURLE_OPERATION_TIMEDOUT);
                    continue;
                }
                uint64_t remaining = io->idle_until - now;
                int wait = remaining > INT_MAX ? INT_MAX : (int)remaining;
                if (timeout < 0 || wait < timeout)
                    timeout = wait;
            }
            short flags = (reading(io) ? POLLIN : 0) | (writing(io) ? POLLOUT : 0);
            if (!flags)
                continue;
            if (reading(io) && io->read_buffered)
                timeout = 0;
            links[count] = io;
            fds[count++] = (struct pollfd){.fd = io->socket, .events = flags};
        }
        pthread_mutex_unlock(&pool->lock);
        int ready = poll(fds, (nfds_t)count, timeout);
        int poll_error = ready < 0 ? errno : 0;
        if (ready > 0 && fds[0].revents)
            drain_wake(pool->wake[0]);
        pthread_mutex_lock(&pool->lock);
        /* The owner can remove and free a link while poll waits. */
        if (pool->revision != revision || pool->stopping)
            continue;
        if (ready < 0) {
            if (poll_error == EINTR)
                continue;
            for (size_t i = 1; i < count; ++i)
                failed(links[i], CURLE_RECV_ERROR);
            continue;
        }
        for (size_t i = 1; i < count; ++i) {
            responses_ws_io *io = links[i];
            if (io->paused || io->failure)
                continue;
            short events = fds[i].revents;
#ifdef AH_WS_IO_TEST
            if (io->block_after_steer_ack && io->delivered_steer_ack && io->ctl_type == CURLWS_PONG)
                events = POLLHUP;
#endif
            if (events & POLLNVAL) {
                failed(io, CURLE_RECV_ERROR);
                continue;
            }
            if (events & (POLLHUP | POLLERR)) {
                /* A peer can hang up between Ping and Pong, leaving read data but
                 * no writable event. Drain input without attempting any more writes. */
                if (io->ctl_type) {
                    io->ctl_type = 0;
                    io->ctl_size = io->ctl_sent = 0;
                }
                if (io->queued < INBOX_FRAMES)
                    read_packet(io);
                if (!io->failure && (!io->read_buffered || io->queued == INBOX_FRAMES))
                    failed(io, events & POLLERR ? CURLE_RECV_ERROR : CURLE_GOT_NOTHING);
                continue;
            }
            if (reading(io) && ((events & POLLIN) || io->read_buffered))
                read_packet(io);
            if (!io->failure && writing(io) && (events & POLLOUT))
                write_packet(io);
        }
    }
    pthread_mutex_unlock(&pool->lock);
    free(fds);
    free(links);
    return NULL;
}

static struct responses_ws_io_pool *open_pool(CURLM *owner) {
    struct responses_ws_io_pool *pool = calloc(1, sizeof(*pool));
    if (!pool)
        return NULL;
    pool->owner = owner;
    if (pthread_mutex_init(&pool->lock, NULL))
        goto free_pool;
    if (pipe(pool->wake))
        goto destroy_mutex;
    for (unsigned i = 0; i < 2; ++i)
        if (fcntl(pool->wake[i], F_SETFL, O_NONBLOCK) < 0 ||
            fcntl(pool->wake[i], F_SETFD, FD_CLOEXEC) < 0)
            goto close_pipe;
    if (!pthread_create(&pool->thread, NULL, pump_main, pool))
        return pool;
close_pipe:
    close(pool->wake[0]);
    close(pool->wake[1]);
destroy_mutex:
    pthread_mutex_destroy(&pool->lock);
free_pool:
    free(pool);
    return NULL;
}

responses_ws_io *responses_ws_io_open(ah_responses *p, CURL *easy, curl_socket_t socket) {
    if (!p->websocket_io)
        p->websocket_io = open_pool(p->multi);
    struct responses_ws_io_pool *pool = p->websocket_io;
    responses_ws_io *io = pool ? calloc(1, sizeof(*io)) : NULL;
    if (!io)
        return NULL;
    *io = (responses_ws_io){.pool = pool, .easy = easy, .socket = socket, .read_buffered = true};
#ifdef AH_WS_IO_TEST
    if (p->loopback) {
        io->release_steer_completion = getenv("AH_TEST_WS_RELEASE_STEER_COMPLETION");
        io->block_after_steer_ack = getenv("AH_TEST_WS_STEER_EOF_BEFORE_DRAIN") != NULL;
    }
#endif
    pthread_mutex_lock(&pool->lock);
    io->next = pool->links;
    pool->links = io;
    ++pool->revision;
    pthread_mutex_unlock(&pool->lock);
    wake_pump(pool);
    return io;
}

void responses_ws_io_close(responses_ws_io *io) {
    if (!io)
        return;
    struct responses_ws_io_pool *pool = io->pool;
    pthread_mutex_lock(&pool->lock);
    responses_ws_io **link = &pool->links;
    while (*link && *link != io)
        link = &(*link)->next;
    if (*link)
        *link = io->next;
    ++pool->revision;
    pthread_mutex_unlock(&pool->lock);
    wake_pump(pool);
    while (io->head) {
        ws_packet *item = io->head;
        io->head = item->next;
        free(item);
    }
    free(io);
}

void responses_ws_io_shutdown(ah_responses *p) {
    struct responses_ws_io_pool *pool = p->websocket_io;
    if (!pool)
        return;
    pthread_mutex_lock(&pool->lock);
    pool->stopping = true;
    pthread_mutex_unlock(&pool->lock);
    wake_pump(pool);
    pthread_join(pool->thread, NULL);
    close(pool->wake[0]);
    close(pool->wake[1]);
    pthread_mutex_destroy(&pool->lock);
    free(pool);
    p->websocket_io = NULL;
}

size_t responses_ws_io_pause(responses_ws_io *io, bool *submitted) {
    if (!io)
        return 0;
    pthread_mutex_lock(&io->pool->lock);
    io->paused = true;
    size_t unreported = io->write.written - io->write.reported;
    *submitted |= io->write.started;
    if (io->write.bytes && !io->write.done && !io->failure)
        io->failure = CURLE_ABORTED_BY_CALLBACK;
    io->write = (ws_write){0};
    pthread_mutex_unlock(&io->pool->lock);
    return unreported;
}

void responses_ws_io_resume(responses_ws_io *io, uint64_t idle_until) {
    pthread_mutex_lock(&io->pool->lock);
    io->paused = false;
    io->idle_until = idle_until;
    pthread_mutex_unlock(&io->pool->lock);
    wake_pump(io->pool);
}

CURLcode responses_ws_io_status(responses_ws_io *io) {
    pthread_mutex_lock(&io->pool->lock);
    CURLcode code = io->failure;
    pthread_mutex_unlock(&io->pool->lock);
    return code;
}

void responses_ws_io_probe(responses_ws_io *io) {
    pthread_mutex_lock(&io->pool->lock);
    io->probe = PROBE_QUEUED;
    ++io->probe_nonce;
    io->probe_sent = 0;
    io->probe_match = false;
    pthread_mutex_unlock(&io->pool->lock);
    wake_pump(io->pool);
}

CURLcode responses_ws_io_probe_result(responses_ws_io *io) {
    pthread_mutex_lock(&io->pool->lock);
    CURLcode code = io->failure ? io->failure : io->probe == PROBE_ACKED ? CURLE_OK : CURLE_AGAIN;
    pthread_mutex_unlock(&io->pool->lock);
    return code;
}

CURLcode responses_ws_io_send(responses_ws_io *io, const char *data, size_t size, size_t *sent,
                              bool *submitted) {
    *sent = 0;
    pthread_mutex_lock(&io->pool->lock);
    ws_write *tx = &io->write;
    CURLcode code = CURLE_AGAIN;
    bool queued = false;
    if (tx->bytes) {
        if (data != tx->bytes + tx->reported || size != tx->size - tx->reported)
            code = CURLE_BAD_FUNCTION_ARGUMENT;
        else {
            *sent = tx->written - tx->reported;
            tx->reported = tx->written;
            if (submitted)
                *submitted |= tx->started;
            bool done = tx->done;
#ifdef AH_WS_IO_TEST
            if (!submitted && io->release_steer_completion &&
                access(io->release_steer_completion, F_OK) < 0)
                done = false;
#endif
            if (done || io->failure) {
                code = done ? CURLE_OK : io->failure;
                *tx = (ws_write){0};
            }
        }
    } else if (io->failure)
        code = io->failure;
    else {
        *tx = (ws_write){.bytes = data, .size = size};
        queued = true;
    }
    pthread_mutex_unlock(&io->pool->lock);
    if (queued)
        wake_pump(io->pool);
    return code;
}

CURLcode responses_ws_io_recv(responses_ws_io *io, char *bytes, size_t *length,
                              struct curl_ws_frame *meta) {
    pthread_mutex_lock(&io->pool->lock);
    ws_packet *item = io->head;
#ifdef AH_WS_IO_TEST
    /* The first text frame after the steer is the fixture's ACK. Keep later frames
     * queued until EOF to exercise a read backlog alongside a failed write notice. */
    if (io->block_after_steer_ack && io->release_steer_completion && io->write.done) {
        if (io->delivered_steer_ack && !io->failure) {
            pthread_mutex_unlock(&io->pool->lock);
            return CURLE_AGAIN;
        }
        if (item && (item->meta.flags & CURLWS_TEXT) && !item->meta.bytesleft &&
            !(item->meta.flags & CURLWS_CONT))
            io->delivered_steer_ack = true;
    }
#endif
    if (!item) {
        CURLcode code = io->failure ? io->failure : CURLE_AGAIN;
        pthread_mutex_unlock(&io->pool->lock);
        return code;
    }
    io->head = item->next;
    if (!io->head)
        io->tail = NULL;
    bool wake = io->queued-- == INBOX_FRAMES;
    pthread_mutex_unlock(&io->pool->lock);
    *meta = item->meta;
    *length = item->meta.len;
    memcpy(bytes, item->bytes, *length);
    free(item);
    if (wake)
        wake_pump(io->pool);
    return CURLE_OK;
}
#endif
