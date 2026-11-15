# Native SDK

`ah/sdk.h` is the embedding entry point for C, C++ and Swift hosts. It owns the
engine, built-in Responses transport and optional local tools, including
`view_image`. Existing session, agent, task and event APIs operate on the same
engine. There is no TUI dependency, extra session store or separate scheduler.

## Build and link

For an existing CMake project, add this repository as a subdirectory and link
`ah::ah`. Subproject builds do not enable the CLI, examples, tests or Python
dependency. For an installed SDK:

```sh
cmake -S . -B build-sdk -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
  -DAH_BUILD_CLI=OFF -DAH_BUILD_EXAMPLES=OFF
cmake --build build-sdk --parallel
cmake --install build-sdk --prefix /path/to/sdk
```

```cmake
find_package(ah CONFIG REQUIRED) # Set CMAKE_PREFIX_PATH to /path/to/sdk.
target_link_libraries(my_app PRIVATE ah::ah)
```

The package exports the native dependency links; applications do not have to list
the private JSON/TOML libraries themselves. The CMake SDK target is a C17 static library.
Use matching headers and rebuild the host when upgrading this pre-1.0 API.

Swift Package Manager can consume the repository directly as the `CHarness`
product. A local dependency is sufficient:

```swift
.package(name: "CHarness", path: "/path/to/C Harness")
// Target dependencies:
.product(name: "CHarness", package: "CHarness")
```

Then `import CHarness`. The package compiles the same C sources and vendors;
it has no remote package dependencies. The [Swift example](../examples/embed.swift)
is compiled and run by the macOS standalone check. C/C++ SDK builds are tested on
macOS and Linux; no Windows or iOS support is claimed.

## Configure once

```c
#include <ah/sdk.h>

ah_sdk_config config;
ah_sdk_config_init(&config);
config.engine.database_path = "sessions.db";
config.engine.observer = on_event;
config.engine.observer_context = host;
config.responses.bearer_token = access_token;
config.local.workspace = workspace;

ah_sdk *sdk = NULL;
ah_error error = {AH_OK, ""};
ah_code code = ah_sdk_open(&config, &sdk, &error);
```

Check `code` before using the SDK. `ah_sdk_engine(sdk)` and
`ah_sdk_local_tools(sdk)` are borrowed handles; `ah_sdk_close(sdk)` is their sole
owner. Config strings are consumed/copied on open. Custom callback contexts must
outlive the SDK. A failed construction closes every component it opened.

The SDK does not discover credentials or log in. The host supplies an authorized
token and may replace it with `ah_sdk_set_token` while idle. The optional existing
host auth adapter can run `ah_host` with a reusable subscription login. Custom
providers are selected through `config.engine.provider`; they remain host-owned.

Workspace tools require an explicit path. Writes require approval by default;
`exec`, subagents, automatic context, skills, MCP and plugins remain off. To enable
subagents, set `config.agents.max_active`; the built-in provider automatically
supplies its async interface. A custom provider must supply its own matching
`config.agents.provider`. Other policies reuse the existing configuration structs:
`automatic_context`/`context`, `extensions`, and optional host-defined
`checks`/`check_count` configured before local tool registration.

## Run, pause and resume

Use `ah_session_create`, `ah_submit` or `ah_submit_content`, then `ah_run` on
the borrowed engine. `ah_session_state` returns the typed state without parsing a
JSON snapshot. A return of `AH_OK` can mean a pause as well as completion:

| State | Host action |
| --- | --- |
| `AH_WAITING_INPUT` | Render the `input.requested` question, then call `ah_reply` with its exact operation ID |
| `AH_WAITING_APPROVAL` | Render the requested action, then call `ah_decide` |
| `AH_OUTCOME_UNKNOWN` | Reconcile the actual effect before calling `ah_resolve` |
| `AH_STOPPED` / `AH_FAILED` | Inspect the error; explicitly call `ah_resume` when appropriate |
| `AH_COMPLETED` | The model turn ended; task acceptance remains a separate verifier result |

After a reply/decision/resolution makes the session ready, call `ah_run` again.
Persist the session ID and reopen the same database to continue across app
restarts. Duplicate command IDs preserve the existing idempotency rules.

One host thread owns the SDK and all ordinary APIs. Run it on a worker thread for
a GUI, and copy/enqueue observer data onto the UI thread. `model.delta` is
provisional streaming text; durable events have nonzero `seq`. Callbacks must not
re-enter owner APIs. Cross-thread cancellation and steering use `ah_cancel`,
`ah_cancel_job` and `ah_steer`; join the active run before closing the SDK. Keep the
SDK at application lifetime when hiding a window should leave work running.

[embed.c](../examples/embed.c) is a complete credential-free C/C++ example;
[host.c](../examples/host.c) uses the managed live provider and event observer.
Both check errors and release the SDK. [Protocol](protocol.md) documents event
payloads, while [task semantics](workflows.md) define verified acceptance.

## Local images

`view_image({"path":"screenshots/page.png"})` reads a stable regular file inside
the workspace and returns a native `input_image` result plus path, MIME, byte count
and content revision. Optional `detail` defaults to `auto`; `original` requires
provider support. MIME is detected from bytes, so the extension is not authority.
PNG, JPEG, GIF and WebP signatures are supported. Image decoding is left to the
provider; this tool does not transcode, resize or crop. It can inspect a crop
created by another tool. Symlinks, parent traversal and special files are refused.

The read is cancellable and may share the bounded READ executor. Workers do not
write SQLite: the owner persists image artifacts and ordered receipts through the
existing multimodal path. Identical bytes share an immutable artifact. Reopening
and replaying the receipt does not reread a changed file.

Each image has an 8 MiB raw hard limit. Encoded tool output must fit
`engine.max_output_bytes`, and the whole expanded model request must fit
`engine.max_context_bytes`. The engine defaults remain 1 MiB for each; larger
images need explicit budgets. For example, the live host example selects 12 MiB
output and 16 MiB context. Oversized media fails explicitly rather than being
truncated or silently lowered in quality.
