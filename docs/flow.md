# Flow

Every engine registers `flow({script})`. The script is the body of an async
JavaScript function. Use `await tools.NAME({...})` to call a registered host
tool or `read_result`. Return a JSON value. Direct tool calls still work.
With a workspace, `test({})` runs its executable `./test` with no arguments
after approval. A missing runner returns a tool error. In this repository
`./test` builds and runs the suite; `./test -R flow` selects flow tests.

The built-in `await tools.web_run({queries})` searches Keenable for up to
100 queries as one child operation. [Batch web search](web-run.md)

For a small complete search, this script edits each matching file once and
runs tests after every edit succeeds:

```js
const page = await tools.find({path: ".", glob: "*.txt", query: "before"});
if (page.truncated || page.scan_truncated) throw new Error("Narrow the search");
const revisions = new Map(page.results.map(row => [row.path, row.revision]));
await Promise.all([...revisions].map(([path, base_revision]) =>
  tools.apply_changes({files: [{path, base_revision,
    edits: [{old_text: "before", new_text: "after"}]}]})
));
return revisions.size ? await tools.test({}) : {edited: 0};
```

A failed child rejects its promise. Use `Promise.allSettled` to inspect partial
successes. A batch settles before the VM receives its results, in issue order.
`Promise.race` does not deliver an early wall-clock winner. Only the final
result goes into model history; child intents, approvals and receipts remain
visible in the journal. Direct JS access to files, credentials, sockets and
processes is unavailable. Command execution inside tools retains host OS authority.

Consecutive independent `write_file` and single-file `apply_changes` calls with
a base revision (or `expect_absent` for creation) can execute concurrently up to
the existing worker limit. Every requested approval binds to one operation.
Overlapping paths, unversioned writes, compound edits and commands retain their
ordered execution. Put all nonoverlapping hunks for the same file in one
`apply_changes` call. Earlier successful writes remain committed if a sibling
fails. Workspace coordination remains authoritative; in coordinated mode
`test` requires exclusive broad authority just like other subprocesses.

The owner commits each batch, its wait and the eligible read intents before
dispatch. Reads use the same worker slots as direct calls. During a run the
isolated helper keeps the script and its outstanding promises in memory. The
owner wakes it over a socket with the newly committed results. The journal stores
the completed child keys for each wake. `ah_work_json` expands these to full receipts. A dependent
`await` starts another wave. Use `Promise.all` to issue independent calls together.

If the helper exits while waiting, a fresh helper reconstructs the script from
committed child receipts and verifies each previous tool, argument and batch size.
Recovery follows the same path after a host restart, approval pause or eviction
from the engine's cache of 16 live helpers. A host death between a write and its
receipt requires explicit reconciliation. Runtime changes that affect replay
must bump the flow work version.

Each script has a 32-child bound and uses the normal tool budgets. Its VM has a
64 MiB heap and a 512 KiB JS stack. Each active phase has a five-second CPU
limit. The host enforces cancellation and the tool deadline. Child history has a 12 MiB payload
limit. Calls in one phase must fit the existing 64 KiB continuation envelope.
`Date`, randomness and timers are unavailable. Store large data outside the
script and page results. Install the trusted helper as described in [SDK](sdk.md).
