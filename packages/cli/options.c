#include "cli.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>

void cli_usage(bool all) {
    puts("ah " AH_VERSION " — durable C agent runtime\n"
         "Usage: ah COMMAND [options]\n"
         "Commands: new run resume approve deny reply resolve inspect list events fork compact\n"
         "          agents send stop rpc submit-content artifact-import artifacts forget vacuum\n"
         "          context workspace\n"
#ifdef AH_BUILD_TUI
         "          tui                Interactive terminal and account setup\n"
#endif
         "\nSession and input:\n"
         "  --model NAME         Model for new session (required by new)\n"
         "  --workspace PATH     Register bounded local file tools\n"
         "  --db PATH            Session database (default ./harness.db)\n"
         "  --session ID         Target session\n"
         "  --prompt TEXT        Submit user text before run\n"
         "  --command-id ID      Stable idempotency key (required with prompt/content)\n"
         "  --content JSON       Typed text/image/file input; excludes --prompt\n"
         "  --instructions TEXT  Additional developer instructions for a new session\n"
         "  --events             Print structured execution events\n"
         "  --no-web-search      Disable the built-in keyless Keenable web_run tool\n"
         "\nBudgets:\n"
         "  --context-bytes N    Request byte limit (default 1 MiB; 4096..64 MiB)\n"
         "  --auto-context       Archive older exchanges; thresholds follow the context budget\n"
#ifdef AH_BUILD_TUI
         "                       Enabled by default in tui\n"
#endif

         "  --tool-timeout-ms N      Tool deadline in milliseconds (default 30000)\n"
         "  --parallel-tools N   Concurrent opt-in read tools (default 4; 0/1 serial; max 16)\n"
         "\nOpt-in capabilities (default off):\n"
         "  --agents N           Fresh-context subagents, N concurrent model requests\n"
         "  --enable-exec        Allow process execution with approval (not sandboxed)\n"
         "  --workflows          Enable host check presets (workspace search is always available)\n"
         "  --coordinate-writes  Require explicit workspace write grants via RPC\n"
         "  --skills             Lazy skill discovery and loading\n"
         "  --mcp                Lazy MCP discovery/calls with approval\n"
         "  --plugins            Discover installed plugin bundles\n"
         "                       Plugin skills/MCP also require --skills/--mcp");
    if (all)
        puts("\nAdvanced configuration:\n"
             "  --search-endpoint URL   Override the Keenable REST endpoint for this host\n"
             "  --endpoint URL       Full Responses endpoint (default OpenAI public endpoint)\n"
             "  --transport MODE    auto (default), http, or websocket for a compatible endpoint\n"
             "  --token-file PATH    Private token/SIWC JSON file; otherwise AH_ACCESS_TOKEN\n"
             "  --request-timeout-ms N       Total HTTP request budget (default 120000)\n"
             "  --connect-timeout-ms N       DNS/connect/TLS budget (default 15000)\n"
             "  --first-output-timeout-ms N  Wait after upload (default 90000)\n"
             "  --stream-idle-timeout-ms N   Stream idle budget (default 30000)\n"
             "                              Zero disables an individual phase timeout\n"
             "  --codex-home PATH    Extension source; does not enable extensions\n"
             "  --extension-context-bytes N  Loaded skill limit (default 131072; min 1024)\n"
             "  --agent-limit N      Maximum children per tree (default 32)\n"
             "  --agent-depth N      Maximum child depth (default 8)\n"
             "  --model-tool-bytes N  Per-result model view (default 16384; 0 disables cap)\n"
             "  --context-trigger N  Archive trigger (default 3/4 of context byte limit)\n"
             "  --context-target N   Archive target (default 1/2 of context byte limit)\n"
             "  --context-reserve N  Request reserve (default 1/8 of limit, capped at 64 KiB)\n"
             "  --keep-exchanges N   Recent exact exchanges (default 3; 0..64)\n"
             "                       Context tuning flags also enable --auto-context\n"
             "  --compact-tokens N   Explicit provider native-compaction capability and threshold\n"
             "  --allow-http-loopback  Allow local HTTP Responses/MCP fixtures only\n"
             "\nCommand-specific options:\n"
             "  --operation ID       Approval/input/unknown-operation identity\n"
             "  --value JSON         Input or reconciliation result\n"
             "  --summary TEXT       Explicit context checkpoint summary\n"
             "  --keep-turns N       Recent complete turns retained by compact (default 4)\n"
             "  --target ID          Recipient for send/stop\n"
             "  --wake               Send a task that can wake an idle child\n"
             "  --artifact-path PATH --mime MIME  Import an immutable local file\n"
             "  --forget-tree        Explicitly delete a complete agent tree from its root");
    else
        puts("\nUse --help-all for advanced tuning and command-specific options.");
    puts("RPC: bounded JSON lines; method/id/session fields. Use steer during a run, reply for "
         "pending input.\n"
         "Credentials: AH_ACCESS_TOKEN or --token-file. OAuth belongs to the host.\n"
         "Secrets are never accepted on the command line.");
}

bool cli_option_number(const char *option, const char *text, uintmax_t minimum, uintmax_t maximum,
                       uintmax_t *out) {
    char *end = NULL;
    errno = 0;
    uintmax_t value = strtoumax(text, &end, 10);
    if (*text < '0' || *text > '9' || errno || *end || value < minimum || value > maximum) {
        fprintf(stderr, "Invalid %s\n", option);
        return false;
    }
    *out = value;
    return true;
}
