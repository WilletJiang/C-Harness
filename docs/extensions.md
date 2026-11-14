# Optional extensions

Extensions are disabled by default. `--skills`, `--mcp` and `--plugins` are independent,
explicit opt-ins for the current process. `--plugins` permits discovering bundles;
their skills and servers still require `--skills` and `--mcp`, respectively.
`--codex-home PATH` selects a source without enabling anything. No extension scan,
server process, network request or skill body is triggered merely by opening an engine.

```sh
./build/ah run --db agent.db --session SESSION_ID --workspace /approved/project \
  --skills --mcp --plugins --command-id task-1 --prompt 'Use the relevant installed tools.'
```

An embedding host initializes `ah_extensions_config`, sets only the desired flags,
then calls `ah_extensions_enable(engine, &config, &error)` before running sessions.
The engine owns extension state and closes child processes/connections at shutdown.
Configuration and paths are copied. Reopening requires opting in again; existing
Codex configuration is read, never modified. Codex model login is separate from
MCP authentication. The optional host adapter retains its legacy Argus Keychain
service and registration path so extracting this repository does not invalidate login.

## Discovery and context

`discover` searches bounded name/description metadata and returns stable IDs with
pagination. `kind` selects `skill`, `mcp`, `plugin` or `all`; `query` filters results.
Pass a discovered plugin ID in `plugin` to expand that bundle. Ordinary discovery
does not start MCP servers, read skill bodies or expand every installed plugin.
The default page contains at most 12 entries; the maximum is 32.

`skill {"id":"…"}` loads one skill into a session reference slot. The full text is
included once in subsequent requests, while the tool receipt contains only its
identity, path and content revision. Loading the same ID again replaces its slot
without adding another copy; changes on disk are picked up by explicit reloading.
`skill {"id":"…","action":"unload"}` removes the slot. References, scripts and
assets are read separately when needed; loading a skill never executes its scripts.
Use `skill` with `action:"read"`, a relative `path` and optional byte `offset`/`limit`
to page text references inside its directory. Traversal or symlinks escaping that
directory are rejected. Binary assets need an appropriate host tool.

Loaded skill snapshots survive context compaction and process restart. They are
included only when skills are enabled. Fresh subagents inherit no loaded slots;
the separate explicit `ah_fork` API copies them along with conversation context.
When the skill budget is exhausted, loading fails before replacing any existing
content. The host/model must unload an unused skill; the runtime does not silently
evict instructions the task may still depend on.

`mcp` connects to one server on first use. `tools`, `resources`, `templates` and `prompts` return
metadata pages; `describe` returns just one tool's schema. `read` fetches a resource
by URI; `get` fetches a prompt by name and argument object. Lists are cached per server
page until `refresh:true` or an engine restart. `next_offset` pages within the current
server page; once exhausted, pass `next_cursor` to fetch the next remote page.
`mcp_call` invokes a tool with a `server`, `name` and `arguments` object. Configured
enabled/disabled tool filters apply both to discovery and execution. The CLI pauses
for approval before external calls, just as for its local write/process tools.

Only these small entrypoint schemas enter the tool namespace; installing more
extensions does not append their catalogs, schemas or instructions to every request.
Identical complete discovery/MCP read results reuse a reference while the original
receipt remains in active context. Compaction invalidates that reuse automatically.
Large tool results keep their exact durable receipt and expose a bounded model view
with `read_result` paging. This includes remote schemas and resource contents.

Default budgets are 2,048 catalog entries, 8 MiB of catalog metadata, 64 KiB per
skill, 128 KiB of serialized loaded-skill context per session, and 1 MiB per MCP
result. The MCP page cache has its own 8 MiB ceiling and at most 32 server clients.
The core's overall request and tool-result budgets still apply. These limits count
bytes, not estimated tokens. `--extension-context-bytes` sets the pinned-skill budget.

## Compatibility and boundaries

The catalog reads Codex TOML configuration and local skill/plugin files. It supports
the Codex compatibility manifest `.codex-plugin/plugin.json` and portable root
`plugin.json`, with plugin skill folders and MCP configuration. Explicitly disabled
entries stay disabled. Discovery returns an availability reason for resources that
cannot be used locally rather than presenting an installed bundle as fully runnable.

MCP supports persistent stdio JSON-RPC and Streamable HTTP with JSON or SSE responses.
It uses the 2025-11-25 initialization protocol and accepts the supported negotiated
2025-06-18 and 2025-03-26 versions. The incompatible 2026-07-28 lifecycle is rejected
explicitly; it is not silently guessed or retried. Environment/static HTTP credentials
and configured process environments are supported. Transport values and credentials
are not part of model-visible catalog metadata.

Codex-hosted app connectors and their private login broker are not a portable MCP
endpoint. Declaring such a connector does not make its service available to another
host. UI assets, browser surfaces, lifecycle hooks, server-initiated sampling and
elicitation likewise require host integration and are not executed by this library.
Remote OAuth setup/refresh belongs to the embedding host; authenticated endpoints can
receive credentials through the supported configuration. A server's external effects
are not a sandboxed transaction: after an ambiguous sent request, the engine records
an unknown outcome and requires reconciliation, never an automatic replay.

Configuration/catalog snapshots last for an engine lifetime. Reopen to discover
installed changes or changed policy; use `refresh:true` for a server's tool lists.
Pinned skill snapshots are intentionally stable until explicitly reloaded/unloaded.

Format references: [Codex skills](https://learn.chatgpt.com/docs/build-skills),
[Codex MCP configuration](https://learn.chatgpt.com/docs/extend/mcp),
[plugin packaging](https://developers.openai.com/plugins/build/plugins),
[MCP lifecycle](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle).
