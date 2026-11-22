# Terminal frontend

Build with a C17 toolchain, Cargo, system terminfo and `pkg-config` access to
`libutf8proc`. CMake fetches the pinned MD4C source. Cargo uses the checked-in
lockfile for the statically linked math renderer.

```sh
cmake -S . -B build-tui -DAH_BUILD_TUI=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-tui --parallel
./build-tui/ah tui --workspace .
./build-tui/ah tui --model MODEL --workspace .
./build-tui/ah tui --session SESSION_ID --db harness.db --workspace .
```

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
`/sessions` opens the session picker, `/history` reads the preceding event page,
and `/pending` opens the operation targeted by a reply or approval.
If a tool stops without a confirmed result, send a new message in the same
session. The next turn sees an error result for that call, and the journal
keeps the original evidence.

`tui_view.h` is the keyboard and frame contract. The terminal thread owns the
TTY and independent journal reader. The execution thread owns the active run.
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
