# Terminal frontend

Build with a C17 toolchain, Cargo, system terminfo and `pkg-config` access to
`libutf8proc`. CMake fetches the pinned MD4C source. Cargo uses the checked-in
lockfile for the statically linked math renderer. On macOS, OpenSSL development
libraries are required by the bundled libcurl, which enables WebSocket for the
OpenAI Responses endpoint in this build.

```sh
cmake -S . -B build-tui -DAH_BUILD_TUI=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-tui --parallel
./build-tui/ah tui
./build-tui/ah tui --model MODEL
./build-tui/ah tui --session SESSION_ID --db harness.db
```

The TUI makes the launch directory available to its local file tools. Use
`--workspace PATH` to choose another directory. Process execution is available
with approval.

Skills, MCP servers and plugins are discovered on demand. Configured MCP tool
calls require approval. Subagents are available with up to four concurrent
model requests, including the main agent. Set `--agents N` to choose a
different limit.

Use a file-backed database. The ChatGPT option opens the browser and uses the
existing [ChatGPT host adapter](../host/README.md) and macOS Keychain. The TUI
profile stores the provider choice and default model for this option. The
host adapter supplies the short-lived access token through a private pipe
and refreshes it as needed before a run.

OpenRouter and Custom accept an API key and a base URL such as
`https://host/v1` or a full Responses endpoint. `/config` reopens the form.
The API profile lives in `$XDG_CONFIG_HOME/ah/tui.json` or
`~/.config/ah/tui.json` with mode 0600. Endpoint changes require a key for the
new endpoint.

`/model` opens the model list, then the selected model's reasoning effort.
Press Esc at the effort step to return to the model list. Selecting the current
model between turns updates its effort for the next turn. Selecting a different
model, or selecting a model while the current turn is paused, opens a new
session after confirmation. Enter a model ID in `/config` if the provider has no
model catalog.
Each session saves its instructions at creation. Start a new session to use
updated built-in instructions.

For CLI-supplied credentials use `AH_ACCESS_TOKEN` or `--token-file`, with
`--endpoint` for a custom endpoint. The default model applies to new sessions.
Submitted messages, completed replies, and tool events are saved in SQLite
under `--db` (default `./harness.db`). Reopen the same database to find the
sessions you previously used.
`/sessions` lists conversations by their opening prompt and highlights the
current one. Move with the arrow keys and press Enter to switch. `/history`
reads the preceding event page, and `/pending` opens the operation targeted by
a reply or approval.
Use `/image PATH` to attach a PNG, JPEG, GIF, or WebP to the next message.
Enter sends the image with the draft text, or on its own if the draft is empty.
Use `/image clear` to remove it before sending.
The TUI automatically archives older complete exchanges as context grows.
`/context` shows the active limits and checkpoint count.
When `ask` supplies choices, use the arrow keys and Enter to reply. Press Esc
or select `Type a reply` to use the composer. `/pending` reopens the choices.
An agent can ask a background question while it continues independent work.
Use `/pending` to answer it. During a run, a normal message steers the current
turn, and `/note TEXT` queues additional context for the next model request.
On a supported WSS session, live steer sends the correction on the existing
connection. If the provider is waiting for a tool result, the regular tool and
approval flow supplies that result before the provider continues.
If a tool stops without a confirmed result, send a new message in the same
session. The next turn sees an error result for that call, and the journal
keeps the original evidence.

`/auto-approve` toggles automatic approval for guarded edits of ordinary files
in the current workspace. It starts off each time the TUI opens. Protected
files, edits that empty an existing file, tool calls with checks, and process
execution require manual approval. Each automatically approved edit records
an `approval.auto` event.

`/yolo on` approves all already-enabled tools for the current workspace and
endpoint and saves that choice for reopening the TUI. Confirm older grants once
for the current tool set. Use `/yolo off` to turn it off.
In the pending picker, `a` approves once and `A` enables YOLO and approves the
pending operation.

## Source layout

- [`tui.h`](tui.h): entry point used by the CLI.
- [`app/`](app/): main loop, commands, session events, and streaming output.
- [`provider/`](provider/): provider configuration, OAuth, and model selection.
- [`terminal/`](terminal/): TTY input and output, terminfo, and Kitty graphics.
- [`view/`](view/): keyboard and frame API, chat composer, pickers, and login form.
- [`render/`](render/): Markdown, tables, and formulas; the Rust crate lives in `render/math/`.

[`view/view.h`](view/view.h) is the keyboard and frame contract. The view has no
terminal I/O.

The terminal thread owns the TTY and independent journal reader. The execution
thread owns the active run.
Completed Markdown blocks enter terminal scrollback while the model streams.
The active fragment redraws above the composer. Durable events are read through
sequence cursors following commit notification.

Markdown tables have framed columns and wrap long cells. When the terminal
cannot fit the minimum column widths, they display labeled records.

Use `$...$` or `\(...\)` for inline math and `$$...$$` or `\[...\]` for
display math. On a terminal that answers the Kitty graphics probe, block equations
use STIX Two Math on transparent PNGs colored against the reported terminal theme.
The TUI queries colors again when a run starts. Existing scrollback images keep
the color they were drawn with.

`AH_TUI_MATH=text` forces character-grid math. `AH_TUI_MATH=dark` and
`AH_TUI_MATH=light` select formula ink for the named background and still require
graphics support and cell dimensions. Inline formulas, simple terminals and
sessions inside tmux use character-grid math. Unsupported formulas or formulas
that exceed the available width retain their LaTeX source.
