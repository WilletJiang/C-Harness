# Security boundary

The harness executes tools registered by its embedding host. The host, provider
adapter, and tool implementation are trusted code. Model output, user-supplied
files, remote responses, and subprocess output are untrusted data.

- No tool dispatch before successful terminal response, argument validation and approval.
- Default local file tools traverse descriptor-relative paths and reject symlinks,
  absolute paths, parent traversal and special files. Workspace grants are deliberate.
- Writes use same-directory temporary files and atomic rename; receipts distinguish
  known failures from uncertain post-effect outcomes.
- Optional `exec` uses argv directly, a minimal environment, closed inherited file
  descriptors, a process group, bounded output, cancellation and reaping.
- The built-in `web_run` sends model-selected queries to Keenable over HTTPS. Returned snippets are untrusted web content. Use `--no-web-search` or `config.search.disabled` in offline or private workloads.
- Flow scripts run in a separate QuickJS helper without an OS module loader.
  The helper and its install location are trusted. It only requests registered
  tools; the owner enforces each actual call's policy and workspace permissions.
  `test` always requires approval: the workspace controls its executable `./test`
  and that process runs with ordinary OS account authority.
- **Process execution is not sandboxed.** Once approved, a program can access anything
  permitted to its OS account, spawn detached children, or access the network. The
  process group is cancellation machinery, not a boundary against hostile programs.
- Do not run untrusted programs under a privileged account. A real isolation backend
  (separate user, sandbox, container or VM) belongs behind the executor contract.
- HTTPS validates certificate and host; redirects and URL credentials are rejected.
  Absent Content-Type is accepted only if the body passes strict SSE and terminal
  validation; a present mismatched type is rejected. Plain HTTP is accepted only
  through an explicit loopback test option. Use trusted
  configured endpoints. Network proxy configuration is inherited from libcurl's host
  environment; a production host should control that environment.
- Provider tokens never enter the model request JSON or tool environment. Debug
  output must not include authorization headers. The host owns OAuth and vault access.
- Databases contain conversation and tool output and are not encrypted by this
  library. Store them on access-controlled/encrypted storage and define retention.
- Observers receive potentially sensitive application content. The default CLI does
  not print provisional deltas unless events/RPC are selected. Secure the host's logs.
- A disconnected UI is not evidence that an operation stopped; a timed-out write is
  not evidence that it failed. Unknown outcomes require target-side reconciliation.
- Skills, MCP and plugin loading are independent opt-ins. Loading skill instructions
  does not execute scripts or hooks. Plugin entries requiring a hosted connector/UI
  are marked unavailable. MCP process tools retain the OS authority of their host;
  cancellation and bounded transport buffers do not constitute an OS sandbox.
- MCP credentials stay in host transport configuration, outside discovery metadata.
  Effectful calls are journaled before dispatch. Automatic POST rewind/replay is
  disabled, including libcurl's internal retry on a failed reused connection.

Third-party library fixes and tool-platform support must be reviewed before each
release. Sanitizer/fuzz success reduces implementation risk but does not prove
absence of bugs. No claim of independent security audit is made.

For a security report, contact the repository maintainer privately before opening
an issue containing credentials, personal data, or an exploit against a live host.
