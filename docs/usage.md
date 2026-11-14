# Using C Harness

## Build and verify

Requires CMake 3.20+, a C17 compiler, libcurl 7.68+ built with asynchronous DNS,
SQLite 3.37+, and pthreads. Python 3 is used by integration tests and the optional host adapter. JSON parsing
uses vendored yyjson 0.12.0. Linux process tools require glibc 2.34+; macOS uses
native spawn actions. Unsupported process isolation primitives fail closed.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/ah_embed
```

Sanitizers: configure a separate build with `-DAH_SANITIZE=ON`. The tests use mock
providers, temporary workspaces, and loopback HTTP only: no credentials, paid API,
or external messages. `scripts/check-standalone.sh` copies this project into a
fresh directory, builds/tests/installs it, and builds an independent C client
against the installed `ah::ah` package.

## An offline session

The `ah_embed` example provides an in-memory fake model and exercises the same
public lifecycle as a live provider. It is a complete embedding example, not a
replacement model or a simulation of the network provider.

## Live provider integration

An embedding host creates an `ah_responses` provider with an endpoint and a
bearer credential, then places `ah_responses_provider(provider)` in `ah_config`.
The host owns OAuth registration, refresh and credential storage. Update the token
between calls with `ah_responses_set_token`. Never give model/tool code access to
refresh credentials. Plan-authorized OAuth tokens and Platform API keys are not
interchangeable credentials; use the appropriate supported authorization flow.

Requests use the durable session UUID as `prompt_cache_key`; a fork gets its own
key. The Responses transport mirrors this key into the HTTP `session-id` header:
ChatGPT subscription routing depends on that header independently of the JSON key.
Routing is derived per request, so a shared transport cannot retain another
session's route. Custom keys must be 1–512 printable non-space ASCII bytes;
absent/null keys omit the header. This supplies stable routing, not a guarantee of cache hits.
Changes to earlier instructions, tools or history can still invalidate prefixes.

The CLI accepts `AH_ACCESS_TOKEN` or an explicitly supplied `--token-file`. A token
file must be an owner-only regular file, containing a bearer token or a SIWC JSON
record with a top-level `access_token`. Tokens are not accepted as CLI arguments.
An absent token permits offline session inspection; inference returns `auth`.

```sh
# After the host has supplied an authorized credential:
./build/ah new --db /private/path/agent.db --model MODEL_SLUG
./build/ah run --db /private/path/agent.db --session SESSION_ID \
  --workspace /approved/workspace --command-id unique-command-id \
  --prompt 'Read source.txt and summarize it.' --events
```

File writes pause for approval in the CLI. Inspect the returned pending operation,
then use `approve` or `deny` with its exact ID and run the session again. Process
execution is disabled unless `--enable-exec` is supplied, and remains approval-gated.

## Long tasks and typed input

Add `--auto-context` to a run or RPC owner to archive older complete model/tool
exchanges at settled boundaries. Exact user inputs, human replies, loaded skills,
and recent exchanges remain available. `--context-trigger`, `--context-target`,
`--context-reserve`, and `--keep-exchanges` customize this opt-in policy; supplying
one also enables it. These settings apply to the current process. Inspect them
with `context` using the same flags, or with the RPC `context` method.

`--compact-tokens N` is a separate explicit provider capability setting and uses
tokens. Structural byte limits do not estimate tokens. When protected inputs and
definitions cannot fit, the runtime still returns `limit` rather than discarding
them. No model name silently enables native compaction.

`new`, `run`, and `submit-content` accept `--content JSON` with native typed
text/image/file parts and a required `--command-id`. It cannot be combined with
`--prompt`. Import local files with `artifact-import`, then select their returned
identifiers in typed content. Importing or listing artifacts never sends them.
`--context-bytes N` changes the complete request byte limit from 1 MiB, up to
64 MiB, when larger selected assets need room. See [content and retention](content.md)
for formats, examples, and explicit permanent deletion commands.

Use `--coordinate-writes --workspace ROOT` for persisted exclusive write grants.
The RPC host must assign paths before work; the flag grants no implicit authority.
See [workspace coordination](workspaces.md) for the setup sequence.

## Deliberate boundaries

Session state, SQLite and events have one owner; `ah_cancel`, `ah_cancel_job` and
`ah_steer` are the cross-thread API entry points. Consecutive independent READ
tools run in bounded batches only when their registration opts in with `parallel_safe`. The default is
four workers (`max_parallel_tools`); 0 or 1 selects serial execution. Writes,
external effects and user questions remain ordered barriers within a session.
Native `exec` and MCP callbacks run as background jobs, allowing other agents to
advance; hosts can opt in additional thread-safe callbacks through `ah/jobs.h`.
Callbacks must not reenter owner APIs or block indefinitely. Independent engines
can run independently against different databases. One engine exclusively
owns a database; multiplex sessions through that owner.

`exec` is **not an OS sandbox**. The file tools reject symlinks and traversal, but
an approved arbitrary executable has the user's OS authority. Browser automation,
remote-node enrollment, mail, calendars, task-specific completion
checks and UI belong to host integrations via the tool callback contract.

Each tool receipt retains its complete captured result in the journal. Its model
view defaults to at most 16 KiB (`max_model_tool_bytes`, 0 disables the extra cap).
Larger results expose an operation reference and UTF-8 preview; `read_result`
retrieves exact pages, including in forked sessions. The view is fixed when the
receipt commits, so later configuration changes do not rewrite past prompts.
These are byte budgets, not token estimates. `read_file` defaults to 200 lines;
use its continuation cursor for complete access to long files. `apply_changes` uses
unique exact replacements for single or multi-file edits. Base revisions are optional;
when supplied, they reject a stale observation. Directory listing goes through enabled
process tools such as `ls`; there is no dedicated listing tool.

Context checkpoint summaries are explicit host inputs; automatic summarization,
semantic memory and provider switching are not silently performed. Text-stream
completion is not proof of a real-world task outcome. A product should verify its
own completion conditions before presenting a task as done.

A live subscription comparison completed three synthetic tasks with both this
runtime and Codex, and exposed a fixed transport compatibility issue. Generated
evaluation outputs were removed after validation; reusable runners and graders
remain in `evals/`. This small evaluation does not establish industry leadership.
Advertised behavior is covered by executable contracts; validation boundaries
are documented.


Skills, MCP and plugins are opt-in. See [extensions](extensions.md) for lazy discovery, Codex compatibility and context budgets.
