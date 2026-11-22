#include "cli.h"
#include "signals.h"
#include "extensions.h"
#include "content.h"
#include "policies.h"
#include "ah/content.h"
#include "ah/responses.h"
#include "ah/tools.h"
#include "ah/agents.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef AH_BUILD_TUI
#include "tui_internal.h"
#include <unistd.h>
#endif

int main(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "--help") || !strcmp(argv[1], "help") ||
        !strcmp(argv[1], "--help-all")) {
        cli_usage(argc >= 2 && !strcmp(argv[1], "--help-all"));
        return 0;
    }
    if (!strcmp(argv[1], "--version")) {
        puts(AH_VERSION);
        return 0;
    }
    const char *command = argv[1], *db = "harness.db", *workspace = NULL;
    const char *model = NULL, *instructions = "", *session = NULL, *prompt = NULL;
    const char *command_id = NULL, *operation = NULL, *value = NULL, *summary = NULL;
    const char *endpoint = "https://api.openai.com/v1/responses", *token_file = NULL;
#ifdef AH_BUILD_TUI
    bool endpoint_explicit = false;
#endif
    bool exec_enabled = false, events = false, allow_http = false, workflows = false;
    ah_search_config search = {0};
    uintmax_t tool_timeout_ms = 0;
    unsigned keep = 4, parallel = 4, agents = 0, agent_limit = 32, agent_depth = 8;
    const char *target = NULL;
    bool wake = false;
    size_t model_tool_bytes = 16384;
    cli_content_options content = {0};
    cli_policy_options policies;
    cli_policy_init(&policies);
    ah_extensions_config extensions;
    ah_extensions_config_init(&extensions);
    for (int i = 2; i < argc; ++i) {
        cli_option_status option = cli_extension_option(&extensions, argc, argv, &i);
        if (option == CLI_OPTION_UNHANDLED)
            option = cli_policy_option(&policies, argc, argv, &i);
        if (option == CLI_OPTION_UNHANDLED)
            option = cli_content_option(&content, argc, argv, &i);
        if (option == CLI_OPTION_INVALID)
            return 2;
        if (option == CLI_OPTION_HANDLED)
            continue;
        const char *arg = argv[i];
        if (!strcmp(arg, "--wake")) {
            wake = true;
            continue;
        }
        if (!strcmp(arg, "--workflows")) {
            workflows = true;
            continue;
        }
        if (!strcmp(arg, "--enable-exec")) {
            exec_enabled = true;
            continue;
        }
        if (!strcmp(arg, "--events")) {
            events = true;
            continue;
        }
        if (!strcmp(arg, "--no-web-search")) {
            search.disabled = true;
            continue;
        }
        if (!strcmp(arg, "--allow-http-loopback")) {
            allow_http = true;
            continue;
        }
        if (i + 1 == argc) {
            fputs("Missing option value\n", stderr);
            return 2;
        }
        const char *v = argv[++i];
        if (!strcmp(arg, "--db"))
            db = v;
        else if (!strcmp(arg, "--workspace"))
            workspace = v;
        else if (!strcmp(arg, "--model"))
            model = v;
        else if (!strcmp(arg, "--instructions"))
            instructions = v;
        else if (!strcmp(arg, "--session"))
            session = v;
        else if (!strcmp(arg, "--prompt"))
            prompt = v;
        else if (!strcmp(arg, "--command-id"))
            command_id = v;
        else if (!strcmp(arg, "--operation"))
            operation = v;
        else if (!strcmp(arg, "--value"))
            value = v;
        else if (!strcmp(arg, "--summary"))
            summary = v;
        else if (!strcmp(arg, "--endpoint")) {
            endpoint = v;
#ifdef AH_BUILD_TUI
            endpoint_explicit = true;
#endif
        } else if (!strcmp(arg, "--search-endpoint"))
            search.endpoint = v;
        else if (!strcmp(arg, "--tool-timeout-ms")) {
            if (!cli_option_number(arg, v, 1, UINT32_MAX, &tool_timeout_ms))
                return 2;
        } else if (!strcmp(arg, "--target"))
            target = v;
        else if (!strcmp(arg, "--agents") || !strcmp(arg, "--agent-limit") ||
                 !strcmp(arg, "--agent-depth")) {
            uintmax_t number;
            bool limit = !strcmp(arg, "--agent-limit");
            if (!cli_option_number(arg, v, 1, limit ? 1024 : 32, &number))
                return 2;
            if (limit)
                agent_limit = (unsigned)number;
            else if (!strcmp(arg, "--agents"))
                agents = (unsigned)number;
            else
                agent_depth = (unsigned)number;
        } else if (!strcmp(arg, "--token-file"))
            token_file = v;
        else if (!strcmp(arg, "--parallel-tools") || !strcmp(arg, "--model-tool-bytes")) {
            uintmax_t number;
            bool concurrency = !strcmp(arg, "--parallel-tools");
            if (!cli_option_number(arg, v, 0, concurrency ? 16 : SIZE_MAX, &number))
                return 2;
            if (!concurrency && number && number < 1024) {
                fprintf(stderr, "Invalid %s\n", arg);
                return 2;
            }
            if (concurrency)
                parallel = (unsigned)number;
            else
                model_tool_bytes = (size_t)number;
        } else if (!strcmp(arg, "--keep-turns")) {
            uintmax_t number;
            if (!cli_option_number(arg, v, 0, 10000, &number))
                return 2;
            keep = (unsigned)number;
        } else {
            fputs("Unknown option\n", stderr);
            return 2;
        }
    }
    if ((exec_enabled || workflows || policies.coordinate_writes) && !workspace) {
        fputs("--enable-exec/--workflows/--coordinate-writes require --workspace\n", stderr);
        return 2;
    }
    if (prompt && !command_id) {
        fputs("--prompt requires --command-id\n", stderr);
        return 2;
    }
    if (!cli_content_options_valid(&content, command, prompt, command_id))
        return 2;
    const char *commands[] = {"new",     "run",     "resume", "approve", "deny", "reply",
                              "resolve", "inspect", "list",   "events",  "fork", "compact",
                              "agents",  "send",    "stop",   "rpc"};
    bool known = cli_content_command(command) || cli_policy_command(command);
    bool tui = false;
#ifdef AH_BUILD_TUI
    tui = !strcmp(command, "tui");
    if (tui && (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) || (model && session))) {
        fputs("ah tui needs a terminal and at most one of --model or --session\n", stderr);
        return 2;
    }
    if (tui && (events || prompt)) {
        fputs("Use ah run or ah rpc for --prompt or --events\n", stderr);
        return 2;
    }
#endif
    known |= tui;
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
        if (!strcmp(command, commands[i]))
            known = true;
    if (!known) {
        fputs("Unknown command\n", stderr);
        return 2;
    }
    if (!strcmp(command, "new") && !model) {
        fputs("--model is required\n", stderr);
        return 2;
    }
    bool standalone = !strcmp(command, "new") || !strcmp(command, "list") ||
                      !strcmp(command, "rpc") || !strcmp(command, "vacuum") ||
                      !strcmp(command, "workspace") || tui;
    if (!standalone && !session) {
        fputs("--session is required\n", stderr);
        return 2;
    }
    char *token = cli_load_token(token_file);
#ifdef AH_BUILD_TUI
    if (tui && token && !*token && !token_file) {
        free(token);
        token = NULL;
    }
    bool has_cli_token = token != NULL;
    const char *token_source = token_file ? "file" : has_cli_token ? "env" : "none";
#endif
    if (token_file && !token) {
        fputs("Cannot read private token file (regular, owner-only permissions required)\n",
              stderr);
        return 2;
    }
    ah_error error = {0};
    ah_responses *responses = NULL;
    ah_responses_config pc;
    ah_responses_config_init(&pc);
    pc.endpoint = endpoint;
    pc.bearer_token = token;
    pc.allow_loopback_http = allow_http;
    ah_code code = ah_responses_open(&pc, &responses, &error);
    if (token) {
        size_t n = strlen(token);
        volatile char *p = token;
        for (size_t i = 0; i < n; i++)
            p[i] = 0;
        free(token);
    }
    if (code != AH_OK) {
        fprintf(stderr, "%s: %s\n", ah_code_name(code), error.message);
        return 1;
    }
    cli_context cli = {.json_events = events, .rpc = !strcmp(command, "rpc")};
    pthread_mutex_init(&cli.output, NULL);
    ah_config config;
    ah_config_init(&config);
    search.allow_loopback_http = allow_http;
    config.search = search;
    if (tool_timeout_ms)
        config.tool_timeout_ms = tool_timeout_ms;
    config.database_path = db;
    if (policies.context_bytes)
        config.max_context_bytes = policies.context_bytes;
    config.max_parallel_tools = parallel;
    config.max_model_tool_bytes = model_tool_bytes;
    config.provider = ah_responses_provider(responses);
    config.observer = cli_observe;
    config.observer_context = &cli;
    code = ah_open(&config, &cli.engine, &error);
    if (code == AH_OK)
        code = cli_policy_enable(cli.engine, &policies, workspace, &error);
    if (code == AH_OK &&
        (extensions.enable_skills || extensions.enable_mcp || extensions.enable_plugins)) {
        extensions.workspace = workspace;
        extensions.allow_http_loopback = allow_http;
        code = ah_extensions_enable(cli.engine, &extensions, &error);
    }
    ah_local_tools *tools = NULL;
    if (code == AH_OK && workspace) {
        ah_local_tools_config tc = {.workspace = workspace,
                                    .enable_exec = exec_enabled,
                                    .approve_writes = true,
                                    .enable_workflows = workflows};
        code = ah_local_tools_open(&tc, &tools, &error);
        if (code == AH_OK)
            code = ah_local_tools_register(tools, cli.engine, &error);
        if (code == AH_OK)
            cli.local_tools = tools;
    }
    if (code == AH_OK && agents) {
        ah_agents_config ac;
        ah_agents_config_init(&ac);
        ac.provider = ah_responses_async_provider(responses);
        ac.max_active = agents;
        ac.max_agents = agent_limit;
        ac.max_depth = agent_depth;
        code = ah_agents_enable(cli.engine, &ac, &error);
    }
    bool watching = code == AH_OK && app_signals_start(cli.engine);
    if (code == AH_OK && !watching) {
        code = AH_IO;
        snprintf(error.message, sizeof(error.message), "Cannot install cancellation handler");
    }
    char id[AH_ID_CAP] = {0};
    char *json = NULL;
#ifdef AH_BUILD_TUI
    if (code == AH_OK && tui)
        code = cli_tui(&cli, responses, db, session, model, instructions, endpoint,
                       endpoint_explicit, has_cli_token, token_source, allow_http, &error);
#endif
    if (code == AH_OK && !tui) {
        if (cli.rpc) {
            int rc = cli_rpc(&cli);
            if (rc)
                code = AH_IO;
        } else if (!strcmp(command, "new")) {
            code = ah_session_create(cli.engine, model, instructions, id, &error);
            session = id;
        } else if (cli_content_command(command))
            code = cli_content_execute(cli.engine, command, session, command_id, &content, &json,
                                       &error);
        else if (cli_policy_command(command))
            code = cli_policy_execute(cli.engine, command, session, &json, &error);
        else if (!strcmp(command, "list"))
            code = ah_sessions_json(cli.engine, &json, &error);
        else if (!strcmp(command, "agents"))
            code = ah_agents_json(cli.engine, session, &json, &error);
        else if (!strcmp(command, "send"))
            code = ah_agent_send(cli.engine, session, target, command_id, prompt, wake, &error);
        else if (!strcmp(command, "stop"))
            code = ah_agent_interrupt(cli.engine, session, target, &error);
        else if (!strcmp(command, "inspect"))
            code = ah_session_json(cli.engine, session, &json, &error);
        else if (!strcmp(command, "events")) {
            cli.json_events = true;
            code = ah_events(cli.engine, session, 0, cli_observe, &cli, &error);
        } else if (!strcmp(command, "resume"))
            code = ah_resume(cli.engine, session, &error);
        else if (!strcmp(command, "approve") || !strcmp(command, "deny"))
            code = ah_decide(cli.engine, session, operation, !strcmp(command, "approve"), &error);
        else if (!strcmp(command, "reply"))
            code = ah_reply(cli.engine, session, operation, value, &error);
        else if (!strcmp(command, "resolve"))
            code = ah_resolve(cli.engine, session, operation, value, false, &error);
        else if (!strcmp(command, "fork")) {
            code = ah_fork(cli.engine, session, id, &error);
            session = id;
        } else if (!strcmp(command, "compact"))
            code = ah_compact(cli.engine, session, summary, keep, &error);
        if (code == AH_OK && prompt && strcmp(command, "send"))
            code = ah_submit(cli.engine, session, command_id, prompt, &error);
        if (code == AH_OK && content.json && strcmp(command, "submit-content"))
            code = ah_submit_content(cli.engine, session, command_id, content.json, &error);
        if (code == AH_OK && !strcmp(command, "run"))
            code = ah_run(cli.engine, session, &error);
        if (!cli.rpc && !json && session && code == AH_OK)
            code = ah_session_json(cli.engine, session, &json, &error);
        if (json) {
            cli_print(&cli, json);
            ah_free(json);
        }
    }
    if (watching)
        app_signals_stop();
    ah_close(cli.engine);
    ah_local_tools_close(tools);
    ah_responses_close(responses);
    pthread_mutex_destroy(&cli.output);
    if (code != AH_OK)
        fprintf(stderr, "%s: %s\n", ah_code_name(code), error.message);
    return code == AH_OK ? 0 : code == AH_CANCELLED ? 130 : 1;
}
