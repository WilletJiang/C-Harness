# Code map and ownership

The installed product is the C library and CLI. Python host/evaluation packages
are optional development and integration tools; they are not runtime dependencies
of the library.

| Area | Owns | Boundary |
| --- | --- | --- |
| `include/ah/` | Public C contracts | No private structs or product-specific UI |
| `src/core/` | Sessions, journal, context, execution/recovery | Calls provider/tool callbacks; one database owner |
| `src/agents/` | Fresh child sessions, owner scheduling, durable mail, joins and tree accounting | Shared tools, independent request state, explicit wake and recovery |
| `src/providers/` | Responses lifecycle, HTTP transport and SSE assembly | Returns a validated complete response; no tool execution |
| `src/tools/` | Local file/process tools and argument validation | Reports results/effect uncertainty to the core |
| `src/extensions/` | Opt-in Codex catalogs, pinned skills and lazy MCP clients | Fixed discovery schemas; no process/network activity before use |
| `src/cli/` | CLI/RPC protocol, credentials and signals | Embeds the public runtime |
| `host/` | Optional ChatGPT OAuth/Keychain adapter | Supplies/refreshes a bearer; keeps refresh credentials private |
| `evals/` | Workloads, shared runner adapters and measurements | Can use private APIs; not a stable embedding interface |
| `tests/`, `host/tests/` | Offline contracts and fixtures | No real inference or user Keychain records |
| `benchmarks/` | Local synthetic overhead measurements | Excludes model inference unless explicitly stated |
| `docs/evaluations/` | Historical results and compact ledgers | Separate from current API/operational guidance |

The Responses provider separates lifecycle/token ownership (`responses.c`) from
HTTP transfer and retry/cancellation behavior (`responses_http.c`). SSE framing
and output-item assembly remain distinct bounded parsers. A transport change must
preserve the successful-terminal barrier and the core's native response history.

Evaluation workloads share `runners/`, `trace.py` and `metrics.py`. Smoke and
endurance runs therefore use one credential boundary, interruption implementation
and token accounting path. Cache studies use the same native Responses transport.
Workload checks and data stay with each workload; generated outputs stay elsewhere.

The root CMake file declares the runtime, CLI, install contract and options.
Component CMake files own optional benchmarks, evaluation drivers and tests. All
runtime executable paths remain under the build root. `BUILD_TESTING=OFF` permits
a pure C build without Python discovery.

Python tools run from the harness root as modules:

```sh
python3 -B -m host.auth status
python3 -B -m evals.smoke.run --help
python3 -B -m evals.endurance.run --help
python3 -B -m evals.cache.run --help
```

The session step is shared by synchronous runs and the subagent scheduler. Tool
intent, authorization, dispatch and receipt handling are shared by inline,
background, resumable and host-created work. `work_store.c` owns checkpoint,
wait and generation transitions; `work_signal.c` owns the bounded durable inbox;
`work_calls.c` and `work_join.c` own keyed composition; `work_cancel.c` preserves
unresolved descendant effects. `work_dispatch.c` selects the runnable frontier
and checks definition compatibility. `work_host.c` exposes host-only roots.
These extend existing operation identities; they do not own a second scheduler.

A parked callback retains JSON state rather than a stack or thread. The host
runs again after delivering a signal or at `ah_work_next_wake`. `observe.c`
opens independent read-only connections without engine locks or recovery; event
batches release database read locks before calling UI observers. Model text
streaming remains provisional until a completed response is committed.

Schema 8 migrates older stores transactionally. Native clients must rebuild for
the extended public structures in version 0.10. See [durable work](durable.md),
[SDK integration](sdk.md), and [recovery semantics](semantics.md).
