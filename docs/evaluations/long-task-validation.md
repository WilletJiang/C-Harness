# Long-task runtime validation — 2026-11-14

Implementation: `5820426`; baseline: `178c69e` (0.6). Compact measurements,
configuration acknowledgments, binary/grader hashes and all attempts are retained
in [the ledger](long-task-validation.json). Generated workspaces, databases, raw
traces, temporary builds and fuzz corpora were removed after this record was saved.

## Correctness and local execution

All 43 contracts pass in Release and ASan/UBSan builds. Focused background-job,
MCP, ownership and retention tests also pass TSan. Content decoding/roundtrip
passed 20,000 libFuzzer/UBSan iterations. The isolated copy/build/test/install
check and an external CMake consumer pass, including the macOS CoreFoundation
framework link. Linux and macOS CI passed for the implementation commit.

An actual runtime test with six large tool results and a human answer reaches the
context limit without automatic maintenance. With maintenance it reaches the
fixed acceptance condition after eight model responses, preserving the exact user
choice and complete recent exchanges. Archived text can be reconstructed through
bounded recall pages, including from a fork. New skill loads preserve the prior
input-array prefix; changed/unloaded skills deliberately change visible context.

A fixed native subprocess fixture produced identical receipts in 548 ms with one
worker and 274 ms with two. This measures overlap of independent local work, not
an end-to-end model improvement. It does not advertise provider-native `async:true`.

Three runs of the existing 100-turn Release microbenchmark on Apple M2/macOS
showed SQLite/FULL P50 ranges of 1.416–2.172 ms for the baseline and 1.531–1.763 ms
for the candidate. With a retained 1 MiB input, ranges were 2.497–3.618 ms and
2.528–2.829 ms. Memory-only short-history P50 was 0.148–0.159 ms versus
0.172–0.354 ms; the added path has a small measurable local cost. These distributions
do not establish a general runtime speedup. They exclude real networking/inference.

## Real subscription checks

Both cohorts requested `gpt-6.1-sol`, `xhigh`, the unchanged `update` workload and
the same independently frozen checker. Time includes startup, execution, shutdown
and grading. A 240-second external ceiling was not disclosed in the prompt.
Each successful cohort has one pair; these are capability/regression checks.

| Cohort | Arm | Accepted | Seconds to acceptance | Model calls | Tool calls | Input | Cached input | Output |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Expanded candidate | 0.6 baseline | Yes | 45.235 | 4 | 5 | 8,060 | 4,224 | 1,497 |
| Expanded candidate | 0.7, auto-context + 4 agent slots | Yes | 54.905 | 6 | 8 | 18,343 | 12,672 | 1,327 |
| Minimal configuration | 0.7 candidate | Yes | 46.551 | 5 | 8 | 12,648 | 9,344 | 1,361 |
| Minimal configuration | 0.6 baseline | Yes | 43.493 | 4 | 3 | 7,971 | 5,376 | 1,285 |

Cached input is a subset of input. The expanded pair ran baseline first; the
minimal pair used recorded seeded order, candidate first. Minimal settings request
auto-context off, agents off and four read workers. The old driver does not
acknowledge those flags; its matching defaults are established from its source,
not from a new self-report. Optional tool schemas still differ between versions.

Every successful attempt passed the exact file/content/preservation checks. No
tool failed, no child was spawned, and no automatic archive was triggered on this
small task. The candidate took more model rounds. Provider-phase totals were
44.754/54.427 seconds in the expanded pair and 43.061/46.081 seconds for baseline/
candidate in the minimal pair; tool interval unions were 0.375/0.315 seconds and
0.301/0.345 seconds. The observed differences are predominantly in model requests,
not local tool time. Round count, generated trajectories and service variation
are not isolated by these small samples.

The candidate was slower in both observed pairs. There is no claim of improved
general task latency, nor a P95 estimate. The new mechanisms establish overlap,
bounded continuation and recovery; broader natural-completion trials are needed
to measure when those capabilities reduce accepted completion time.

Four earlier attempts failed before any tool work. A minimal diagnostic request
identified `subscription_sharing_usage_limit_exceeded`; no usage receipt was
provided, so token counts remain unknown. After the user adjusted the subscription
sharing allowance, subsequent attempts completed normally. These failed attempts
remain in the ledger and are not treated as model-quality or speed evidence.
The diagnostic also exposed a nested `error.code` parsing omission; its regression
test now verifies limit/auth classification without propagating remote messages.
