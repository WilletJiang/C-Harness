# Coordinated workspace writes

Workspace coordination is opt-in and persists with the engine database. It gives each
session exclusive ownership of explicit files or directory prefixes. A fresh session,
fork or child starts without grants. Reads remain available.

The host enables it with `ah_workspace_enable(engine, workspace, error)` and assigns
paths using `ah_workspace_assign(engine, session, paths, count, false, error)` from
`ah/workspace.h`. `ah_workspace_json` returns the persisted policy and owners. Passing
zero paths and `broad=false` releases ownership. The workspace is identified by its
filesystem device and inode; a tool bound to another workspace is rejected even if
its relative path is identical.

Paths must be canonical and workspace-relative. `src` covers `src/file.c`, but not
`src2`; `.` covers the entire workspace. Absolute paths, empty components, `.`/`..`
components and symlinks are rejected. A grant can name a new file whose parent already
exists. Grants cannot overlap, including between parent and child agents. Ownership
updates are atomic and require an idle engine with no running or unresolved effects.
They survive reopen; completion or cancellation does not silently release them.

On macOS, conflict checks respect each parent directory's case sensitivity and
conservatively reserve Unicode-equivalent names. This includes files that do not
yet exist and aliases of directory prefixes. Authorization resolves existing
filesystem names through their descriptors; it does not grant access solely
because two strings collate equally. A missing final filename must use the
granted spelling until created. Transfers and `spawn.write_paths` always use the
exact spelling stored in the grant. Case-sensitive Linux paths retain their
byte-sensitive matching.

A host can split a parent's planned work into separate initial grants, for example
`src/parser.c` and `src/reader.c`. The parent can then call:

```json
{"name":"parser","message":"Implement the parser change and verify it.",
 "write_paths":["src/parser.c"]}
```

The optional `spawn.write_paths` moves exact existing parent grants to the child in
the same transaction as creation. It does not copy authority or conversation history.
If any path is unavailable, the whole spawn is rejected without an orphan child or a
partial transfer. To delegate part of a larger prefix, the host must first replace it
with disjoint grants. `ah_workspace_transfer` moves an exact grant between a parent
and its direct child, including returning it after the child's effects are settled.

Coordinated `write_file` requires either the `base_revision` returned by `read_file`
or `find`, or `expect_absent: true` when creating a new file. Every `apply_changes`
file requires `base_revision`. Missing or stale revisions reject the operation before
that file is written. `expect_absent` uses atomic create-if-absent, so a newly appearing
file cannot be overwritten. The revision fields also work when coordination is off;
legacy calls without an expectation retain their previous behavior in that mode.

The owner thread authorizes native file operations before dispatch. Workers receive
only the immutable requirement to check revisions, and do not query SQLite. This
prevents runtime agents with disjoint grants from overwriting one another's work.
It is not a filesystem sandbox or an atomic compare-and-swap against outside writers;
external file edits can still race a replacement. Existing descriptor traversal and
file identity checks detect observed changes and refuse symlinks.

`exec`, host check processes, edits that include checks, and arbitrary effect callbacks
cannot be constrained by a list of paths. In coordinated mode they require an explicit
`broad=true` assignment. Broad authority excludes every other session's write ownership
and waits for executing effects; unknown outcomes must be resolved first. It is never
inherited through spawn. Read callbacks remain trusted to honor their host-declared
read-only contract. Host task verifiers are trusted host code, run synchronously after
background effects settle, and are outside these tool path grants.

## CLI and RPC host setup

Start one owner with `ah rpc --db agent.db --workspace /approved/workspace
--coordinate-writes`. The workspace is pinned immediately, but the initial
ownership list is empty. Create a session, use its returned ID to assign work,
then submit and run:

```json
{"id":1,"method":"create","model":"MODEL_SLUG"}
{"id":2,"method":"workspace_assign","session":"SESSION_ID","paths":["src/parser.c","src/reader.c"],"broad":false}
{"id":3,"method":"submit","session":"SESSION_ID","command":"work-1","text":"Update the two assigned files."}
{"id":4,"method":"run","session":"SESSION_ID"}
```

`workspace` inspects all owners. `workspace_transfer` takes `from`, `to`, and
`path`, with the same parent/direct-child checks as the C API. To release grants,
send `workspace_assign` with `paths:[]` and `broad:false`. Broad process authority
requires a separate explicit `broad:true` assignment and exclusive ownership.
All ownership mutations return `busy` while an RPC run is active.

`ah workspace --db agent.db` is a read-only inspection command. Coordination and
grants persist across CLI invocations; merely passing `--workspace` registers
tools and never creates grants.
