# Evaluation tools

Run Python commands from the harness root. These packages share execution adapters,
trace redaction and usage accounting. Default CTest tests use fixtures and loopback
servers only; live inference is always an explicit command.

| Location | Responsibility |
| --- | --- |
| `smoke/` | Three small, fixed tasks and independent validators |
| `endurance/` | Workboard task, original checks, additional audit and parallel comparison |
| `cache/` | Controlled prefix/key/layout experiments |
| `transport/` | Optional Python HTTP/WS protocol experiment, incremental history and recovery |
| `runners/` | C executable and Codex app-server lifecycle, cutoff and wire checks |
| `trace.py`, `metrics.py` | Content-safe traces and token subset/cumulative accounting |
| `drivers/` | Optional native C evaluation executables and request decorator |

## Build and authorize

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAH_LIVE_EVAL=ON
cmake --build build --parallel
python3 -B -m host.auth login
```

The optional [host adapter](../host/README.md) reuses the subscription login in
macOS Keychain and refreshes it when needed. `exec -- COMMAND ...` passes only a
short-lived access token to a trusted evaluation process. It does not create a
Platform API key. Workspaces and tools must not receive refresh credentials.

## Small tasks

```sh
python3 -B -m host.auth exec -- python3 -B -m evals.smoke.run \
  --driver build/live_driver --output /private/fresh-smoke \
  --model gpt-6.1-sol --effort low
```

`--runner harness|codex|both` defaults to both. `--case` selects `reconcile`,
`intervals` or `update`; otherwise all three run. Each case/runner uses a fresh
workspace. The command returns nonzero when a task fails validation or execution.

| Task | Independent checks |
| --- | --- |
| reconcile | Revisions, dates, decimal rounding, joins, totals and unchanged source data |
| intervals | Boundary and malformed inputs, no mutation, generators and seeded union checks |
| update | Exact edit scope, consistent JSON/configuration, preserved historical files |

Both runners now use the same subscription Responses route and shared adapters.
Older small-task reports used the Codex CLI login route; those historical timings
are not directly interchangeable with this unified runner. System prompts, tools
and context policies still differ between implementations.

## Parallel long tasks

```sh
python3 -B -m host.auth exec -- python3 -B -m evals.endurance.run \
  --driver build/live_driver --output /private/fresh-endurance \
  --model gpt-6.1-sol --effort xhigh --duration 600 --repetitions 3
```

The task implements the complete Workboard API, durable storage, query engine,
CLI, regression tests and documentation. The prompt does not disclose the external
observation ceiling. No follow-up forces a stopped model to continue. A wire probe
checks Codex's requested model/effort before inference; two live file-read preflights
must pass before a barrier releases all six task runs. Probe usage stays separate.

The task, specification and grader hashes are recorded before execution. A copy
of the grader runs outside the model workspaces. Each run is graded even after a
failure or cutoff. Preserve failed attempts and label reruns explicitly. Natural
completion, correctness, missing deliverables and externally stopped work must be
reported separately. The additional audit runs with:

```sh
python3 -B -m evals.endurance.quality /absolute/path/to/workspace
```

Do not modify submitted implementations before grading. Generated tests should also
be rerun independently. The additional audit includes storage-layout probes; review
a fixture failure before assigning it to the implementation. The unspecified
Conflict/NotFound precedence case remains an unscored observation.

## Cache controls

```sh
python3 -B -m host.auth exec -- python3 -B -m evals.cache.run \
  --driver build/cache_driver --output /private/fresh-cache --rounds 2 --parallel
```

Fixed reference data supports cold, identical and growing requests. Equal-length
fresh group IDs isolate trials. Variants compare JSON keys and instruction layout;
the optional phase uses three concurrent clients. These are protocol probes, not
a task-quality benchmark. Keep a baseline binary to isolate a transport change.

The study records accepted parameters, actual model/effort, key echoes, canonical
input fingerprints, usage and optional diagnostics. A returned `unavailable`
diagnostic is not a classified hit or miss. Missing usage remains unknown, and
failures are not automatically replaced.

The native agent driver also accepts these evaluation-only environment variables:

- `AH_EVAL_CACHE_KEY=session|omit`: the durable UUID by default, mirrored into
  HTTP `session-id`; omission removes both.
- `AH_EVAL_CACHE_DIAGNOSTICS=0|1`: request previous-response comparisons (default 0).
- `AH_EVAL_TRACE_REQUESTS=0|1`: emit prepared requests to the evaluator (default 0).

`evals.trace.Trace` fingerprints prepared requests before persistence. Do not save
raw driver stdout when request tracing is enabled. Ordinary model/tool events still
contain task data. Token summaries include cached tokens within input and reasoning
within output; never add these subsets twice. Interrupted calls may omit usage.

## Transport experiment

The optional [transport package](transport/README.md) compares pooled HTTP/SSE,
full-history WebSocket and same-socket incremental WebSocket. It has a separate
Python 3.11+ / `aiohttp` dependency and 14 offline tests; it is not required by the
C build or default CTest suite. It reuses the host login and retains compact metrics
without raw model text or credentials. The production C provider remains HTTP/SSE.
See the [measured results and limits](../docs/evaluations/websocket-evaluation.md).

## Boundaries and records

C tools use bounded native file access and an evaluation-only Codex sandbox wrapper
for process execution. Codex uses an isolated app-server home with external skills,
plugins, memory and child agents disabled. The same endpoint does not make prompts,
tool APIs or context windows identical. Parallel repetitions may share provider
caches and load. Small task counts cannot establish broad superiority.

Keep generated code, databases and raw traces outside the repository. Remove them
after retaining the necessary compact measurements; retain the Keychain login.
Historical reports and ledgers live in [docs/evaluations](../docs/evaluations/README.md).
The layout changed after those experiments; their recorded source hashes refer to
the original commits. Prompt/specification contents remain unchanged.

## Repeated baseline/candidate acceptance comparison

Keep separately built baseline and candidate drivers, then run the fixed short
(`update`) and medium (`intervals`, `reconcile`) tasks in paired trials:

```sh
python3 -B -m host.auth exec -- python3 -B -m evals.compare \
  --baseline-driver /private/baseline/live_driver \
  --candidate-driver /private/candidate/live_driver \
  --output /private/fresh-paired-report --model gpt-6.1-sol --effort low \
  --repetitions 3 --seed 417 --order alternating
```

Add `--long` for Workboard or use repeated `--case` selections. `--timeout` and
`--long-timeout` are external observation ceilings, never added to prompts. The
same model and effort apply to both arms. `--baseline-runner codex` optionally
uses the existing Codex adapter instead of a baseline C driver. Model/tool counts
unavailable from that adapter remain null; observed actions and reported usage
are separate fields.

`--env AH_EVAL_AUTO_CONTEXT=1`, `--env AH_EVAL_AGENTS=4` and
`--env AH_EVAL_PARALLEL_TOOLS=4` set shared C-driver controls. The defaults preserve
the existing driver behavior: context management and agents off, four parallel
read workers. Agent runs decorate root and child requests identically and dispatch
sandboxed exec as background jobs. Per-arm `--baseline-env` / `--candidate-env`
make deliberate differences explicit. Only documented feature flags are accepted;
inherited `AH_EVAL_*` options are removed, and token values never enter manifests.
Drivers report applied controls; older binaries that ignore flags remain visibly
unconfirmed. Codex does not consume these C-driver controls.

The manifest records binary hashes, grader hashes, exact input hashes, prompts,
seed and actual pair order before work starts. Graders are copied outside model
workspaces, made read-only and hash-checked before and after use. Every attempt is
retained, including exceptions, failed graders and externally censored work. The
endurance acceptance rule includes both original checks and the existing audit,
plus preserved inputs, requested deliverables and independently rerun generated
tests; the audit is frozen before these trials.

`results.json` contains every attempt and its observed usage; `summary.json`
reports acceptance, censoring, per-case accepted completion times and paired time
ratios only when both attempts pass. Time to acceptance includes runner startup,
execution, shutdown and host grading. Correct files found after a timeout remain
censored, with no accepted time. Missing usage is unknown or explicitly incomplete;
cached/reasoning tokens remain subsets. No P95 or broad speed claim is inferred
from a few trials. Compare acceptance first and preserve failed attempts when
repeating a study. Offline orchestration tests use injected runners; driver tests
use loopback fixtures only.

## Image-based long tasks

`python3 -B -m host.auth exec -- python3 -B -m evals.endurance.run --driver build/live_driver --output /private/fresh-visual --model gpt-6.1-sol --effort xhigh --visual --duration 1100 --repetitions 3`
adds three native image inputs to the Workboard release task. All six runs start
behind a barrier; no cutoff or follow-up is disclosed to the models. A dependency
graph, parameter table and execution timeline are available only as PNGs. The
frozen grader separates API correctness, image transcription, replayed integration,
modified input behavior, documentation and generated tests. The host image fixture
generator uses Pillow and the macOS Menlo font; generated solutions require only
Python's standard library. Images and generated workspaces are evaluation artifacts,
not runtime dependencies. Both runners receive the same PNG files in the same order.
