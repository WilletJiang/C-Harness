#include "app.h"
#include "signals.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t resize_fd = -1;

void tui_notify(int fd) {
    if (fd >= 0) {
        ssize_t n;
        do {
            n = write(fd, "x", 1);
        } while (n < 0 && errno == EINTR);
    }
}

static void resize(int sig) {
    (void)sig;
    int saved = errno;
    tui_notify(resize_fd);
    errno = saved;
}

static void detect_workspace(tui_app *app) {
    char raw[512];
    if (getcwd(raw, sizeof(raw))) {
        const char *home = getenv("HOME");
        if (home && *home) {
            size_t hlen = strlen(home);
            if (!strncmp(raw, home, hlen) && (raw[hlen] == '/' || raw[hlen] == 0))
                snprintf(app->cwd, sizeof(app->cwd), "~%s", raw + hlen);
            else
                snprintf(app->cwd, sizeof(app->cwd), "%s", raw);
        } else {
            snprintf(app->cwd, sizeof(app->cwd), "%s", raw);
        }
    }
    // Walk up at most 6 directories to read .git/HEAD without spawning a subprocess
    char dir[512];
    if (!getcwd(dir, sizeof(dir)))
        return;
    for (int depth = 0; depth < 6; ++depth) {
        char head_path[600];
        snprintf(head_path, sizeof(head_path), "%s/.git/HEAD", dir);
        FILE *f = fopen(head_path, "r");
        if (f) {
            char line[256];
            if (fgets(line, sizeof(line), f)) {
                size_t len = strlen(line);
                while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
                    line[--len] = 0;
                const char *prefix = "ref: refs/heads/";
                if (!strncmp(line, prefix, strlen(prefix)))
                    snprintf(app->branch, sizeof(app->branch), "%s", line + strlen(prefix));
                else if (len >= 7)
                    snprintf(app->branch, sizeof(app->branch), "%.7s", line);
            }
            fclose(f);
            return;
        }
        char *slash = strrchr(dir, '/');
        if (!slash || slash == dir)
            break;
        *slash = 0;
    }
}

static void sync_status(tui_app *app) {
    app->status.model = app->model;
    bool auto_approve = ah_local_tools_auto_approve(app->cli->local_tools);
    const char *mode = app->approve_all ? "yolo" : auto_approve ? "auto" : "";
    if (app->effort[0] && mode[0]) {
        snprintf(app->model_label, sizeof(app->model_label), "%s %s   %s", app->model,
                 app->effort, mode);
        app->status.model = app->model_label;
    } else if (app->effort[0]) {
        snprintf(app->model_label, sizeof(app->model_label), "%s %s", app->model, app->effort);
        app->status.model = app->model_label;
    } else if (mode[0]) {
        snprintf(app->model_label, sizeof(app->model_label), "%s   %s", app->model, mode);
        app->status.model = app->model_label;
    }
    app->status.session = app->session;
    app->status.state = app->state;
    app->status.cwd = app->cwd;
    app->status.branch = app->branch;
    app->status.turn = app->turn;
    app->status.notice = !app->notice[0] && !app->running && !strcmp(app->state, "outcome_unknown")
                             ? "Tool result unconfirmed. Send a new message." : app->notice;
    app->status.image_name = app->image_name;
    app->status.replying = app->reply_operation[0] != '\0';
    pthread_mutex_lock(&app->preview_lock);
    memcpy(app->network_label, app->network_status, sizeof(app->network_label));
    pthread_mutex_unlock(&app->preview_lock);
    app->status.activity = app->active_tool[0] ? app->active_tool :
                           app->running && app->network_label[0] ? app->network_label : NULL;
    app->status.active_tools = app->active_tools;
    app->status.has_pending = app->pending_status[0] != 0;
    app->pending_card = (tui_pending_card){
        .active = !app->running && app->pending_status[0] != 0,
        .kind = app->pending_status,
        .operation = app->operation,
        .tool = app->pending_tool,
        .summary = app->pending_summary,
    };
    app->status.pending = app->pending_card.active ? &app->pending_card : NULL;
    app->status.picker = app->picker.open ? &app->picker : NULL;
    app->auth.has_token = app->has_token;
    app->auth.token_source = app->token_source;
    app->auth.endpoint = app->endpoint;
    app->auth.model = app->default_model;
    app->auth.config_path = app->config_display;
    app->auth.error = app->auth_error;
    app->status.auth = app->auth.open ? &app->auth : NULL;
}

static void *run_worker(void *opaque) {
    tui_app *app = opaque;
    app->run_code = app->chatgpt ? tui_oauth_refresh(app->responses, &app->run_error) : AH_OK;
    if (app->run_code == AH_OK)
        app->run_code = ah_run(app->cli->engine, app->session, &app->run_error);
    atomic_store(&app->worker_done, true);
    tui_notify(app->wake[1]);
    return NULL;
}

bool tui_start_run(tui_app *app, ah_error *error) {
    tui_graphics_colors(&app->graphics);
    memset(&app->run_error, 0, sizeof(app->run_error));
    app->network_status[0] = 0;
    app->network_permission_denied = false;
    atomic_store(&app->worker_done, false);
    if (pthread_create(&app->worker, NULL, run_worker, app) != 0) {
        error->code = AH_IO;
        snprintf(error->message, sizeof(error->message), "Cannot start execution thread");
        return false;
    }
    app->notice[0] = 0;
    app->active_tool[0] = 0;
    app->active_tools = 0;
    snprintf(app->state, sizeof(app->state), "running");
    app->status.notice_error = false;
    app->running = app->status.busy = app->status.steering = true;
    return true;
}

static void finish_run(tui_app *app) {
    pthread_join(app->worker, NULL);
    app->running = app->status.busy = app->status.steering = false;
    app->active_tool[0] = 0;
    app->active_tools = 0;
    tui_stream_abort(app, "Response interrupted");
    tui_stream_free(app);
    ah_error error = {0};
    char pending[AH_ID_CAP];
    memcpy(pending, app->pending_session, sizeof(pending));
    ah_code code = tui_snapshot(app, app->session, false, &error);
    if (code == AH_OK && pending[0] && strcmp(pending, app->session) &&
        (!app->operation[0] || strcmp(app->pending_session, app->session)))
        code = tui_snapshot(app, pending, false, &error);
    if (app->run_code == AH_AUTH && app->chatgpt && !app->network_permission_denied) {
        app->has_token = false;
        tui_auth_open(app, false);
        snprintf(app->auth_error, sizeof(app->auth_error), "%.255s", app->run_error.message);
    } else if (app->run_code == AH_UNCERTAIN && !strcmp(app->state, "outcome_unknown")) {
        app->status.notice_error = false;
        app->notice[0] = 0;
    } else if (app->run_code != AH_OK)
        tui_show_error(app, app->run_code, &app->run_error);
    else if (code != AH_OK)
        tui_show_error(app, code, &error);
}

static bool setup_pipe(int fd[2]) {
    if (pipe(fd) != 0)
        return false;
    for (unsigned i = 0; i < 2; ++i) {
        if (fcntl(fd[i], F_SETFL, O_NONBLOCK) < 0 || fcntl(fd[i], F_SETFD, FD_CLOEXEC) < 0) {
            close(fd[0]);
            close(fd[1]);
            fd[0] = fd[1] = -1;
            return false;
        }
    }
    return true;
}

static int minimum_timeout(int a, int b) {
    return a < 0 ? b : b < 0 || a < b ? a : b;
}

static bool terminal_loop(tui_app *app) {
    struct pollfd ready[] = {
        {STDIN_FILENO, POLLIN, 0}, {app->wake[0], POLLIN, 0}, {app_shutdown_fd(), POLLIN, 0},
        {-1, POLLIN, 0},
    };
    while (!app->quit) {
        uint64_t now = ah_now_ms();
        tui_terminal_size(&app->cols, &app->rows);
        tui_graphics_resize(&app->graphics, app->cols, app->rows);
        int history_wait = app->restore_history ? tui_graphics_wait(&app->graphics, now) : -1;
        if (history_wait < 0)
            tui_restore_history(app);
        tui_stream_render(app);
        tui_resume_yolo(app);
        tui_ask_maybe_open(app);
        sync_status(app);
        tui_frame frame;
        if (!tui_view_frame(app->view, &app->status, app->cols, app->rows, now, &frame))
            return false;
        tui_terminal_draw(&app->tty, &frame);
        if (ferror(stdout))
            return false;
        int timeout = minimum_timeout(tui_view_timeout(&app->status, now),
                                      tui_input_timeout(&app->input, now));
        timeout = minimum_timeout(timeout, history_wait);
        if (app->refresh_events)
            timeout = minimum_timeout(timeout, 20);
        ready[3].fd = app->oauth_fd;
        if (poll(ready, sizeof(ready) / sizeof(*ready), timeout) < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (ready[2].revents)
            break;
        if (ready[3].revents)
            tui_oauth_read(app);
        if (ready[1].revents & POLLIN) {
            char discard[4096];
            (void)read(app->wake[0], discard, sizeof(discard));
            app->refresh_events |= atomic_exchange(&app->journal_dirty, false);
        }
        if (app->refresh_events)
            tui_refresh(app);
        if (app->catalog_loading && atomic_load(&app->worker_done))
            tui_models_ready(app);
        if (app->running && !app->refresh_events && atomic_load(&app->worker_done))
            finish_run(app);
        now = ah_now_ms();
        sync_status(app);
        tui_input_expire(&app->input, now, tui_handle_key, app);
        if (ready[0].revents & POLLIN) {
            char bytes[4096];
            ssize_t count = read(STDIN_FILENO, bytes, sizeof(bytes));
            if (count <= 0 && (count == 0 || errno != EINTR))
                break;
            if (count > 0)
                tui_input_read(&app->input, bytes, (size_t)count, now, tui_handle_key, app);
        } else if (ready[0].revents) {
            break;
        }
    }
    return true;
}

ah_code cli_tui(cli_context *cli, ah_responses *responses, const char *database,
                const char *session, const char *model, const char *instructions,
                const char *endpoint, bool endpoint_explicit, bool has_cli_token,
                const char *token_source, bool allow_loopback_http, ah_error *error) {
    tui_app app = {.cli = cli,
                   .responses = responses,
                   .wake = {-1, -1},
                   .oauth_fd = -1,
                   .instructions = instructions,
                   .has_token = has_cli_token,
                   .endpoint_explicit = endpoint_explicit,
                   .allow_loopback_http = allow_loopback_http};
    if ((endpoint && strlen(endpoint) >= sizeof(app.endpoint)) ||
        (model && strlen(model) >= sizeof(app.default_model))) {
        error->code = AH_INVALID;
        snprintf(error->message, sizeof(error->message), "Endpoint or model exceeds the TUI limit");
        return AH_INVALID;
    }
    atomic_init(&app.worker_done, false);
    atomic_init(&app.journal_dirty, false);
    if (pthread_mutex_init(&app.preview_lock, NULL) != 0) {
        error->code = AH_IO;
        snprintf(error->message, sizeof(error->message), "Cannot initialize terminal event buffer");
        return AH_IO;
    }
    detect_workspace(&app);
    tui_config_init_paths(&app);
    snprintf(app.endpoint, sizeof(app.endpoint), "%s",
             endpoint && *endpoint ? endpoint : "https://api.openai.com/v1/responses");
    snprintf(app.token_source, sizeof(app.token_source), "%s",
             token_source && *token_source ? token_source : "none");
    if (model)
        strcpy(app.default_model, model);
    tui_config_load(&app);
    app.status.notice = app.notice;
    app.status.preview = app.preview;
    app.status.commands = tui_commands(&app.status.command_count);
    struct sigaction previous, on_resize = {0};
    on_resize.sa_handler = resize;
    sigemptyset(&on_resize.sa_mask);
    bool watching = false;
    ah_code code = AH_IO;
    if (!setup_pipe(app.wake) || !(app.view = tui_view_create()) || !tui_terminal_open(&app.tty)) {
        snprintf(error->message, sizeof(error->message), "Cannot initialize the terminal frontend");
        goto done;
    }
    code = ah_reader_open_with_writer(database, &app.reader, error);
    if (code != AH_OK)
        goto done;
    code = ah_reader_set_busy_timeout(app.reader, 0, error);
    if (code != AH_OK)
        goto done;
    tui_terminal_size(&app.cols, &app.rows);
    if (session) {
        if (strlen(session) >= sizeof(app.session)) {
            code = AH_INVALID;
            snprintf(error->message, sizeof(error->message), "Invalid session ID");
            goto done;
        }
        strcpy(app.session, session);
        code = tui_snapshot(&app, app.session, true, error);
        if (code != AH_OK)
            goto done;
        if (!app.default_model[0])
            strcpy(app.default_model, app.model);
        if (!app.has_token) {
            tui_auth_open(&app, true);
        } else {
            tui_print_banner(&app);
            app.restore_history = true;
        }
    } else if (!app.has_token || !app.default_model[0]) {
        tui_auth_open(&app, true);
    } else {
        code = ah_session_create(cli->engine, app.default_model, instructions, app.session, error);
        if (code != AH_OK)
            goto done;
        code = tui_snapshot(&app, app.session, true, error);
        if (code != AH_OK)
            goto done;
        tui_print_banner(&app);
    }
    if (app.session[0])
        tui_models_sync(&app);
    resize_fd = app.wake[1];
    watching = sigaction(SIGWINCH, &on_resize, &previous) == 0;
    if (!watching) {
        code = AH_IO;
        snprintf(error->message, sizeof(error->message), "Cannot observe terminal resizes");
        goto done;
    }
    cli->event_context = &app;
    cli->event_sink = tui_observe;
    app.input.graphics = &app.graphics;
    tui_graphics_start(&app.graphics, app.cols, app.rows, ah_now_ms());
    if (app.auth.open && app.chatgpt && app.default_model[0]) {
        ah_error auth_error = {0};
        if (tui_oauth_start(&app, app.default_model, false, &auth_error) != AH_OK)
            snprintf(app.auth_error, sizeof(app.auth_error), "%.255s", auth_error.message);
    }
    code = terminal_loop(&app) ? AH_OK : AH_IO;
    if (code == AH_IO)
        snprintf(error->message, sizeof(error->message), "Terminal input or output failed");
done:
    if (app.catalog_loading)
        pthread_join(app.worker, NULL);
    yyjson_doc_free(app.catalog);
    tui_oauth_cancel(&app);
    tui_auth_close(&app);
    tui_picker_close(&app);
    tui_terminal_close(&app.tty);
    if (app.running) {
        while (!atomic_load(&app.worker_done)) {
            ah_cancel(cli->engine);
            struct pollfd ready = {app.wake[0], POLLIN, 0};
            if (poll(&ready, 1, 30) > 0) {
                char discard[4096];
                (void)read(app.wake[0], discard, sizeof(discard));
            }
        }
        pthread_join(app.worker, NULL);
    }
    tui_stream_free(&app);
    cli->event_sink = NULL;
    cli->event_context = NULL;
    resize_fd = -1;
    if (watching)
        sigaction(SIGWINCH, &previous, NULL);
    if (app.wake[0] >= 0)
        close(app.wake[0]);
    if (app.wake[1] >= 0)
        close(app.wake[1]);
    free(app.input.pasted);
    tui_view_destroy(app.view);
    ah_reader_close(app.reader);
    pthread_mutex_destroy(&app.preview_lock);
    error->code = code;
    return code;
}
