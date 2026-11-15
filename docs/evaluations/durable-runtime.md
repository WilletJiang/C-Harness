# Durable runtime evaluation

Measured on 2026-11-15 using an Apple M2, 16 GiB RAM, macOS 26.5.2,
Apple Clang 21.0.0, and Release builds. The baseline is commit
`f7b34537f220707af86bfba84633d6757116c64b`; the candidate is version 0.10.0,
schema 8, including the final recovery, cancellation and readiness fixes.

The question was whether ordinary tools retain low overhead when durable work is
unused, and whether parked operations release worker and CPU resources.
[The benchmark](../../benchmarks/durable.c) uses deterministic callbacks and an
in-memory SQLite database, with no network or model inference.
[The JSON record](durable-runtime.json) contains final raw samples, development
measurements and environment metadata. Builds and other project tests were
paused during the final paired measurement.

## Ordinary execution

The same source was compiled with `AH_BENCH_ORDINARY_ONLY` against both revisions.
Five process pairs alternated order; each process discarded one warmup and
measured seven runs. Each run created, submitted, executed and checked 64 sessions,
each with one tool callback and two mock model responses.

| Build | Median of 35 runs | Range of process medians | Range of individual runs |
| --- | ---: | ---: | ---: |
| Baseline | 25.685 ms | 22.561–26.224 ms | 22.500–27.157 ms |
| Current | 27.256 ms | 26.693–29.708 ms | 25.878–30.424 ms |

The final observed median increase was **6.116%, or 24.547 µs per session**.
Earlier paired observations ranged from about 3% to 7%; desktop scheduling noise
precludes a universal overhead claim. This added durability does not make an
ordinary one-step callback faster. A separate candidate run measured ordinary
and immediate-terminal durable tools at 27.208 and 35.613 ms per 64 sessions.

A SQL diagnostic counted exactly 47,104 statements in each build for eight
64-session runs: no additional durable-table queries on the unused-work path.
Event inserts accounted for 46,592 extra SQLite VM instructions from the retained
`events_session` index. That index supports bounded historical reads by session;
the diagnostic does not isolate its precise wall-time cost.

## Parked worker operations

Both rows ran 256 operations in one session with one worker slot. Each operation
executed, parked on a signal, then executed once more after delivery. Parking time
includes session creation and one start/run per operation. Resume time includes
signal delivery and the indicated host ingress pattern.

| Resume ingress | Park | Resume | Resident increase | Total threads before / parked | Idle process CPU / wall |
| --- | ---: | ---: | ---: | ---: | ---: |
| Signal then run, per operation | 200.095 ms | 142.160 ms | 672 KiB | 1 / 1 | 0.011 / 55.017 ms |
| All signals then one run | 200.121 ms | 74.809 ms | 656 KiB | 1 / 1 | 0.012 / 55.014 ms |

Each row is one observation. Both leave zero worker threads while parked and
near-resolution idle CPU use. Resident changes include SQLite and session state.
The host patterns expose readiness at different times; their ratio is not a
general agent-task speedup. Every operation's final value, error flag, callback
count and receipt count is checked.

The corrected frontier initially scanned parked leaf operations through repeated
C/SQL lookups. Signal delivery now publishes `work_ready` in the same transaction,
and SQL skips idle read leaves while retaining timer, subtree and effect barriers.
The development runs preserve the slower intermediate implementation rather than
hiding that regression. There is still a scan of pending records within a session;
this benchmark does not claim constant-time dispatch for arbitrary graph sizes.

## Validation and reproduction

The final source passed 52 ASan/UBSan suites, five TSan suites, 48 standalone
Release suites, installed C/C++ clients, nested CMake embedding, and a separate
Swift Package consumer. New regressions cover SIGKILL, deferred parent receipts,
sibling receipt preservation, scoped cancellation, fail-fast draining, stale
continuations, version migration, bounded restart loops, signal races and expiry,
nested approval, read-only observer isolation, and snapshot/event cursors.

Configure Release with `-DAH_BENCHMARK=ON`, build `bench_durable`, and run it.
For comparison, compile the same source with `AH_BENCH_ORDINARY_ONLY` against the
older headers and libraries. Keep compiler, optimization and workload fixed.
