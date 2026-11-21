# Versioned work and task acceptance (0.4)

These facilities reduce mechanical model turns without changing the default
provider or turning the session engine into a general workflow language. Actual
speed depends on model tool selection; the [paired smoke study](evaluations/execution-evaluation.md)
did not establish a general speedup. Select the extra tool profile before a session.

## Tool profile and context

Workspace tools include `find`, `read_file`, `view_image`, `write_file`,
`apply_changes`, and the approval-gated `test` command. A host can add fixed
check presets with `enable_workflows=true` before registration. The first
registration attempt freezes presets, including on partial failure; recreate the
tools/engine on such a failure rather than changing published callback policies.

`find({"mode":"files"})` lists workspace-relative regular file paths, including binary and
empty files. Use this to discover the tree without knowing a search term. For example,
`{"mode":"files","path":"src","glob":"*.c","max_results":20}` returns sorted
`results: [{"path":"…"}]`, `total_files`, a snapshot and a continuation offset. Listing
does not read file bodies; its snapshot tracks the matching names and search settings.
For content search, pass a single-line literal `query` (mode `content`, the default).
This returns merged line excerpts, SHA-256 file revisions and byte continuations.
Both modes default `path` to `.`. The glob matches workspace-relative paths;
`*` can span directories. Continue with the same settings, `next_offset` and
`snapshot`. A stale snapshot requires restarting at zero.

Discovery applies workspace-local `.gitignore` patterns before descending into
directories and counts only glob-matching regular files against the file cap.
Nested rules override parent rules; an excluded parent must be re-included
before its children can be re-included. Pass `include_ignored:true` to inspect
ignored paths. This is filesystem discovery: no Git index, global excludes or
repository configuration is consulted. File permissions and write grants remain
independent of ignore rules. Host verification and check fingerprints still
walk their declared inputs independently of discovery filters.

Content search inspects up to 2,048 matching files and 32 MiB of content; listing
can enumerate up to 16,384 entries. Both have independent depth (32 below the
scope) and visited-entry (16,384) bounds. Pages contain at most 100 results.
A partial scan sets `scan_truncated`; narrow the scope before concluding there
are no matches. Symlinks, special files, Git metadata and in-flight write files
are excluded. Content search also skips non-text and over-1-MiB files. Ignore
rule inputs are bounded at 64 KiB per file, 1 MiB per walk and 1,024 active
patterns. Unreadable or excessive rule input fails the search explicitly.

The native implementation searches for a hit before deriving line context. It
tracks only six recent line starts, skips repeated hits within a matching line,
and avoids allocating an entire file's line index. Source buffers are sized to
the observed file length; short reads and observed changes are rejected. ASCII
validation uses unaligned-safe word checks while preserving full UTF-8 validation.
Full content hashing and snapshot validation remain enabled, including on files
without a match. See [measurements](evaluations/find-performance.md) for the bounded
callback speedup and its distinction from model-task latency.

`read_file` now reports a full-file SHA-256 revision for files up to 1 MiB; larger
files remain paged but have `revision:null`. Pages account for the separate model
view budget, including JSON escaping and the result envelope. The engine may replace
an identical successful read with a reference only if the complete earlier page
is still in an active context item. The original full result remains retrievable
with `read_result`. `force:true` returns the page again. Compaction invalidates
visibility through the active-item join; forks start with an empty reuse index.
No earlier receipt is rewritten, and a generic truncated preview is never considered
proof that the model saw the full page. Reads still inspect current source content;
this is context deduplication, not an unchecked filesystem read cache.

## Editing and finite combinations

Use `apply_changes` for both one replacement and multi-file/multi-hunk edits.
A no-op reports `unchanged` without rewriting. There is no separate `edit_file` tool.
Use `write_file` for creation or deliberate replacement.

`apply_changes` accepts `files`, each containing `path`, optional `base_revision`
and an `edits` array of `{old_text,new_text}` hunks. A single replacement uses one
file and one hunk. Supplying a revision from `read_file`/`find` enforces that base;
without it, unique matching text is the precondition, so no extra hash-reading turn
is required. Observed changes between source read and commit are still rejected.
Every old text must occur
exactly once in the original file; hunks may arrive in any order but cannot overlap.
All files are prepared and checked before writing starts. Limits: 16 files, 64 hunks
per file, 1 MiB per hunk string and 1 MiB source/resulting file (UTF-8 bytes).
The engine's separate argument-byte budget still applies. A stale/ambiguous source
returns its revision and a bounded source preview. This structured hunk interface
does not add a model-native/custom `apply_patch_call` wire format.

A single replacement uses the same interface, without a mandatory hash lookup:

```json
{"files":[{"path":"client.py","edits":[{"old_text":"TIMEOUT_MS = 800","new_text":"TIMEOUT_MS = 1200"}]}]}
```

An optional `checks` array names up to eight host presets to run after **all** edits
succeed. Unknown or duplicate check names fail before any file write. Each file is
atomically replaced, but the group is not atomic. Receipts separately report
`application`, per-file results, applied count, checks and `outcome_unknown`.
A failed/timed-out check does not mean the patch needs applying again.

Effect callbacks receive an owner-thread-only `checkpoint` function. It persists
`tool.checkpoint` events (maximum 128 per callback, 16 KiB each). Compound tools
record each file/check start and finish. Interrupted executing operations continue
to become `outcome_unknown`; inspect phase evidence, reconcile actual effects with
`ah_resolve`, and never retry the whole group blindly. Parallel read callbacks have
no checkpoint function. No cross-process compare-and-swap or automatic rollback
over external edits is claimed.

## Host checks

`ah_local_tools_add_check` copies an `ah_check_config`: name/description, fixed JSON
argv array, explicit JSON input-path array, timeout, environment revision, approval
policy and optional cacheability. Models can select names; they cannot replace argv,
inputs or permissions. Commands use the minimal exec environment and may have effects,
so checks are external-effect barriers. `enable_exec` need not expose arbitrary exec
merely to run fixed host commands. Any approval-required preset conservatively gates
the selected-check tool and combined apply tool.

Inputs can be files (each at most 1 MiB) or bounded directories. Directory revisions
cover encountered topology, file contents and identity/mode/owner/size/mtime/ctime metadata, reject incomplete scans/nonregular
entries, and share the traversal exclusions above. Keep the session database and
check-generated outputs outside input trees, or explicitly declare source inputs.
The host must include the full dependency closure and relevant toolchain/environment
in its declaration. A false declaration cannot be repaired by a hash function.
Access time is intentionally excluded; a check that depends on it needs its own
host fingerprint/freshness policy. Metadata hashing can cause conservative misses.
Clock/network-dependent checks should remain uncached or have a host revision that
actually changes with their dependencies.

Checks fingerprint inputs before and after execution. Changed inputs produce `stale`,
not pass. Only stable passes whose finish checkpoint and result encoding succeeded
enter the optional in-memory cache. Unknown results clear that cache. Restarting the
host drops it; acceptance revalidates source revisions even on a cache hit. Failures
include process output and up to three bounded source excerpts for common compiler
or Python diagnostics, restricted to declared inputs. Full captured results remain
in ordinary operation receipts; capture limits and truncation are explicit.

## Optional task controller

The public `ah/task.h` API is separate from ordinary `ah_run` semantics:

1. Configure/register tools and trusted checks.
2. Create a fresh session; obtain a verifier (`ah_local_tools_verifier` supports one
   to eight presets already authorized by the host, without per-operation approval)
   or supply host callbacks for other domains. Approval-required presets remain
   available through the ordinary gated check tool; the adapter refuses to bypass them.
3. Call `ah_task_create` with an immutable JSON contract, verifier definition digest,
   total model/tool/repair limits and timeout; then submit the user request.
4. Call `ah_task_run`. It returns at ask/approval, failures, limits or acceptance.
   Use the existing reply/approval/reconciliation APIs before continuing.
5. Read `ah_task_json`. Only `state:accepted` represents host-verified task acceptance.

The contract is appended once to stable session instructions. A completed model turn
is a candidate; the verifier's policy must match the attached digest, it must return
`ok:true` without error/uncertainty, and pre/post input fingerprints must match.
The host verifier defines semantic quality: a model-authored test or file existence
alone is not proof of a broader task. Include documentation, examples and missing
deliverables in the host's acceptance conditions when the task requires them.

Failed verification supplies its structured observations to a bounded repair turn.
Two failures with the same input revision stop for lack of progress. Task counters
are charged atomically with ordinary model/tool intents and survive new submissions,
resume and process restart. A verification recipe counts as one additional tool
attempt. Repair intent is persisted before a deterministic idempotent command ID,
closing the crash window between accounting and submission. Forks copy context but
do not inherit a task controller; attach a new task only to a fresh session.

Wall budget includes waiting and downtime, using a persisted Unix deadline converted
to monotonic time during each run; host clock changes affect cross-restart accounting.
Callbacks must honor cancellation/deadline. On a verifier crash/uncertain effect,
task state becomes `unknown`; `ah_task_reconcile` requires host evidence and allows
verification again, never grants acceptance. Re-running an accepted task checks that
its source fingerprint still matches; a changed source requires a new task. A saved
accepted receipt is evidence about its recorded revision, not a perpetual guarantee.

## Measurement and compatibility

The existing observer receives provisional, content-free `runtime.metrics` events
for request assembly, model calls, individual tool intervals, commits and verification.
Durations use monotonic microseconds; parallel intervals must be combined, not summed
as elapsed task time. Read reuse is separately counted. Metrics never enter model
context, are not durable acknowledgments and must not trigger observer API reentry.

The optional Responses observer reports per-attempt transport timings, byte counts,
connection reuse and errors. Presence flags distinguish unobserved from zero. First
content requires nonempty generated text/arguments; a terminal event alone still
does not authorize tools. The live evaluation driver records both measurement streams.

Version 0.3 extends public structs: rebuild C clients. Schema 1/2 databases migrate
transactionally to schema 3, adding task and observation tables. Older runtimes reject
schema 3. Version 0.4 preserves those C structs and schema 3 but changes the model
tool interface: `list_directory` and `edit_file` are removed, `search` becomes `find`,
and `apply_changes` is available in the base profile with optional revisions. No
aliases preserve removed names. Saved pending calls to removed tools, or calls with
the older apply schema, fail the normal availability/policy check before effects;
historical receipts are not rewritten. Default C builds require no new package.
WebSocket remains gated: the tested
system libcurl 8.7.1 has exported WS APIs but no compiled `ws/wss` protocol support.
Early dispatch, provider-native asynchronous tool delivery and raw/custom patch
protocols remain unimplemented experiments. Host background jobs for native exec
and MCP are available independently of these synchronous workflow combinations.
