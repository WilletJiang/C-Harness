# Runtime unification validation

The executor and session-step changes were compared with `49e2ab7` using the
same `benchmarks/parallel.c`, copied into the baseline before compilation. Both
arms use Release builds with AppleClang 21 on macOS arm64. Each run checks two
model requests, four exact tool results and ordered durable receipts. The
latency fixtures run 40 measured samples after two warmups; warmed 4 KiB file
reads run 100. Full samples are in [runtime-unification.json](runtime-unification.json).

| Workload, four workers | Store | Before p50 | After p50 |
| --- | --- | ---: | ---: |
| Four background READs, synthetic 20 ms each | Memory | 95.228 ms | 27.023 ms |
| Four background READs, synthetic 20 ms each | SQLite DELETE/FULL | 113.146 ms | 30.750 ms |
| Four ordinary parallel READs, synthetic 20 ms each | Memory | 26.646 ms | 27.175 ms |
| Four warmed 4 KiB file reads | Memory | 0.893 ms | 0.929 ms |
| Four warmed 4 KiB file reads | SQLite DELETE/FULL | 7.490 ms | 6.319 ms |

The main improvement is overlapping independent background READs within one
session. Ordinary parallel READs already overlapped. Small memory-only reads
remain slightly slower in this sample; these measurements do not establish a
general task or model latency improvement. An initial implementation doubled
capture/publication commits and regressed file-backed reads. The final executor
captures currently finished results and publishes the ready prefix in one
transaction, without delaying a completed result to wait for another worker.

Permanent regression tests cover mixed foreground/background reads, one shared
capacity across sessions and nested owner yields, write/approval/input barriers,
actual worker-creation failure, cancellation and deadlines, interrupted processes,
failed receipt transactions, both out-of-order completion patterns and recovery
without duplicate callbacks. Sync and async providers are checked against identical
user/tool input histories across approval, Ask, steering, cancellation/resume,
unknown-outcome resolution and exhausted model budgets.
Stale async provider callbacks cannot emit buffered deltas after a generation
change or stop; reported usage is still retained and the handle is released.

Native results have separate tests for explicit format selection, malformed
content, output/context bounds, UTF-8 previews retaining media, artifact rollback,
v6-to-v7 migration, early capture/reopen, late receipts, fork retrieval, access
boundaries and retention. Actual loopback MCP calls verify mixed text/image/PDF
conversion; plain JSON and text-only MCP keep their existing representation.

A real subscription request through the C engine and Responses transport used
`gpt-6.1-sol` with provider-default reasoning. A tool returned a generated image
and PDF; the model correctly identified orange/cyan halves and the PDF code
`TULIP-4827`, which were withheld from the prompt. This used two model calls,
one tool call, 592 input tokens, 39 output tokens and 11.393 seconds. It verifies
native media consumption, not general visual reasoning quality or speed.

Local validation passed 46 Release CTest suites, 42 ASan/UBSan suites, five
targeted TSan suites, standalone build/install plus an external CMake consumer,
and 20,000 deterministic fuzz iterations each for content decoding and SSE.
After the final cancellation and worker-path checks, the six affected sanitizer
suites and all five TSan suites passed again.

Reproduce the local measurements with `-DAH_BENCHMARK=ON`, then run
`bench_parallel --file-store`. No subscription is required for these benchmarks.
Temporary builds, media fixtures and the live probe are removed after validation;
the existing host credential remains in the system vault.
