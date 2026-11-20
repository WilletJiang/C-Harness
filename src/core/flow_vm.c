#include "flow_vm.h"
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

static bool sibling(char path[PATH_MAX]) {
#ifdef __APPLE__
    uint32_t size = PATH_MAX;
    char binary[PATH_MAX];
    if (_NSGetExecutablePath(binary, &size) || !realpath(binary, path))
        return false;
#else
    ssize_t n = readlink("/proc/self/exe", path, PATH_MAX - 1);
    if (n <= 0 || n >= PATH_MAX - 1)
        return false;
    path[n] = 0;
#endif
    char *slash = strrchr(path, '/');
    if (!slash || (size_t)(slash - path) + sizeof("/ah-flow-vm") > PATH_MAX)
        return false;
    strcpy(slash, "/ah-flow-vm");
    return access(path, X_OK) == 0;
}

static int launch(const char *path, int socket, pid_t *pid) {
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc)
        return rc;
    rc = posix_spawnattr_init(&attr);
    if (rc) {
        posix_spawn_file_actions_destroy(&actions);
        return rc;
    }
    rc = posix_spawn_file_actions_adddup2(&actions, socket, 0);
    if (!rc)
        rc = posix_spawn_file_actions_adddup2(&actions, socket, 1);
    if (!rc)
        rc = posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
#ifdef __APPLE__
    if (!rc)
        rc = posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#elif defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 34)
    if (!rc)
        rc = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
#else
    rc = ENOTSUP;
#endif
#else
    rc = ENOTSUP;
#endif
    char *argv[] = {(char *)path, NULL}, *env[] = {"TZ=UTC", "LC_ALL=C", NULL};
    if (!rc)
        rc = posix_spawn(pid, path, &actions, &attr, argv, env);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    return rc;
}

void core_flow_drop(ah_engine *e, core_flow_cell *cell) {
    core_flow_cell **p = &e->flow_cells;
    while (*p && *p != cell)
        p = &(*p)->next;
    if (!*p)
        return;
    *p = cell->next;
    close(cell->fd);
    pid_t child;
    do {
        child = waitpid(cell->pid, NULL, WNOHANG);
    } while (child < 0 && errno == EINTR);
    if (child == 0) {
        kill(cell->pid, SIGKILL);
        while (waitpid(cell->pid, NULL, 0) < 0 && errno == EINTR) {
        }
    }
    free(cell);
}
void core_flow_close(ah_engine *e) {
    while (e->flow_cells)
        core_flow_drop(e, e->flow_cells);
}

core_flow_cell *core_flow_lookup(ah_engine *e, const ah_tool_call *call) {
    for (core_flow_cell *cell = e->flow_cells; cell; cell = cell->next)
        if (!strcmp(cell->operation, call->operation_id)) {
            if (!cell->next_generation || cell->next_generation == call->generation)
                return cell;
            core_flow_drop(e, cell);
            break;
        }
    return NULL;
}

ah_code core_flow_start(ah_engine *e, const char *operation, core_flow_cell **out, ah_error *err) {
    const char *path = e->flow_runner_path;
    char candidate[PATH_MAX];
    if (!path && sibling(candidate))
        path = candidate;
#ifdef AH_FLOW_INSTALLED
    if (!path)
        path = AH_FLOW_INSTALLED;
#endif
    if (!path)
        return ah_fail(err, AH_IO, "Install ah-flow-vm alongside the host or set flow_runner_path");
    unsigned count = 0;
    for (core_flow_cell *c = e->flow_cells; c; c = c->next)
        if (++count == 16) {
            core_flow_drop(e, c);
            break;
        }
    core_flow_cell *cell = calloc(1, sizeof(*cell));
    if (!cell)
        return ah_fail(err, AH_IO, "Cannot allocate flow cell");
    int fd[2] = {-1, -1};
#ifdef SOCK_CLOEXEC
    int type = SOCK_STREAM | SOCK_CLOEXEC;
#else
    int type = SOCK_STREAM;
#endif
    int rc = socketpair(AF_UNIX, type, 0, fd) < 0 ? errno : 0;
    for (int i = 0; !rc && i < 2; i++) {
        if (fd[i] < 3) {
            int moved = fcntl(fd[i], F_DUPFD_CLOEXEC, 3);
            if (moved < 0) {
                rc = errno;
                break;
            }
            close(fd[i]);
            fd[i] = moved;
        }
        if (fcntl(fd[i], F_SETFD, FD_CLOEXEC) < 0)
            rc = errno;
    }
#ifdef SO_NOSIGPIPE
    int yes = 1;
    if (!rc && setsockopt(fd[0], SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes) < 0)
        rc = errno;
#endif
    if (!rc)
        rc = launch(path, fd[1], &cell->pid);
    if (fd[1] >= 0)
        close(fd[1]);
    if (rc) {
        if (fd[0] >= 0)
            close(fd[0]);
        free(cell);
        return ah_fail(err, AH_IO, "Cannot start trusted ah-flow-vm helper");
    }
    cell->fd = fd[0];
    snprintf(cell->operation, sizeof cell->operation, "%s", operation);
    cell->next = e->flow_cells;
    e->flow_cells = cell;
    *out = cell;
    return AH_OK;
}

static ah_code transfer(int fd, void *bytes, size_t n, bool writing, const ah_tool_call *call,
                        ah_error *err) {
    char *p = bytes;
    while (n) {
        uint64_t now = ah_now_ms();
        if (call->deadline_ms && now >= call->deadline_ms)
            return ah_fail(err, AH_LIMIT, "Flow deadline exceeded");
        if (call->cancelled && call->cancelled(call->cancel_context))
            return ah_fail(err, AH_CANCELLED, "Flow cancelled");
        int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        ssize_t done = writing ? send(fd, p, n, flags) : recv(fd, p, n, MSG_DONTWAIT);
        if (done > 0) {
            p += done;
            n -= (size_t)done;
            continue;
        }
        if (done < 0 && errno == EINTR)
            continue;
        if (done >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            return ah_fail(err, AH_IO, "Flow helper disconnected");
        int ms = 20;
        if (call->deadline_ms && call->deadline_ms - now < (unsigned)ms)
            ms = (int)(call->deadline_ms - now);
        struct pollfd event = {.fd = fd, .events = writing ? POLLOUT : POLLIN};
        if (poll(&event, 1, ms) < 0 && errno != EINTR)
            return ah_fail(err, AH_IO, "Cannot wait for flow helper");
    }
    return AH_OK;
}

ah_code core_flow_exchange(core_flow_cell *cell, const ah_tool_call *call, const char *input,
                           char **json, ah_error *err) {
    size_t n = strlen(input);
    if (!n || n > 16 * 1024 * 1024)
        return ah_fail(err, AH_LIMIT, "Flow input exceeds helper limit");
    uint32_t header = htonl((uint32_t)n);
    ah_code rc = transfer(cell->fd, &header, sizeof header, true, call, err);
    if (rc == AH_OK)
        rc = transfer(cell->fd, (void *)input, n, true, call, err);
    if (rc == AH_OK)
        rc = transfer(cell->fd, &header, sizeof header, false, call, err);
    if (rc != AH_OK) {
        cell->disconnected = rc == AH_IO;
        return rc;
    }
    n = ntohl(header);
    if (!n || n > 4 * 1024 * 1024)
        return ah_fail(err, AH_LIMIT, "Invalid flow output size");
    char *out = malloc(n + 1);
    if (!out)
        return ah_fail(err, AH_IO, "Cannot allocate flow output");
    rc = transfer(cell->fd, out, n, false, call, err);
    if (rc != AH_OK) {
        cell->disconnected = rc == AH_IO;
        free(out);
        return rc;
    }
    out[n] = 0;
    *json = out;
    return AH_OK;
}
