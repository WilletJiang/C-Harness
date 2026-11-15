# Image-based Sol xhigh long-task comparison — 2026-11-15

All six outputs passed the same 76 independent checks. The harness completed its
authored tests and documentation in all three runs; Codex did so in two. **No run
ended naturally within the 1,100-second observation window.** These results measure
quality and reported token usage at a common cutoff, not full completion latency.
The [ledger](vision-sol-xhigh.json) preserves per-run evidence and original scores.

## Setup

The C runtime is version 0.8, commit `8b0a43c`; only evaluation adapters were extended
to attach PNGs. Codex app-server is 0.164.0. Both requested `gpt-6.1-sol`, `xhigh`,
standard service tier, via the same authorized subscription Responses endpoint.
Offline wire verification confirmed Codex's model, effort and three native image
parts; both image paths use `detail=auto`. Live preflights succeeded separately; their tokens are excluded from the task table
and retained separately in the ledger.
The harness responses confirmed the requested model and effort.

Each runner received three fresh copies of the Workboard release task, now with
an image-only dependency graph, task parameter table and 35-step timeline. The
images specify 12 jobs, 16 claim results and 46 durable events. The deliverables
include the complete queue/query/CLI implementation, image transcription, a reusable
demo, focused tests and release documentation. All six runs started behind one
barrier. The ceiling was fixed before launch to fit the available access-token
lifetime; it was not disclosed to the models. There were no follow-up prompts,
network retries, task interventions or repairs to generated code.

Skills, MCP, plugins and subagents were disabled. System prompts, tools and context
handling still differ. This compares these configured runners, not the full Codex
Desktop product. Initial files and graders were hashed before launch. The extra
integration grader reruns the demo with modified payload values and checks actual
exports, avoiding reliance on the model's completion report.

## Quality at the cutoff

| Run | API + boundary audit | Image + integration | Authored tests | Release README |
| --- | ---: | ---: | --- | --- |
| Harness 1 | 50/50 | 26/26 | 39 pass | Written |
| Harness 2 | 50/50 | 26/26 | 38 pass | Written |
| Harness 3 | 50/50 | 26/26 | 46 pass | Written |
| Codex 1 | 50/50 | 26/26 | 50 run, one error | Original stub |
| Codex 2 | 50/50 | 26/26 | 35 pass | Written |
| Codex 3 | 50/50 | 26/26 | 45 pass | Written |

Codex 1's failing test creates a job with default `available_at=0`, then claims at
`now=-2` and dereferences the result. The specification requires `None` there. A
separate probe confirmed its implementation returns `None`, claims at time zero,
and handles the intended heartbeat test when creation explicitly uses
`available_at=-2`. This is an unfinished authored test, not a demonstrated Store
defect. Its README remained the 63-byte placeholder. Test counts do not establish
coverage quality. All six preserved the protected specifications and images.

The first grader pass incorrectly scored 31/33 base checks for every run: its CLI
subprocess inherited the grader directory and could not import `workboard`. Only
the subprocess working directory was corrected; assertions stayed unchanged.
The corrected grader was frozen separately and applied uniformly to all six
unchanged outputs, yielding 33/33. Both versions and scores are retained. A new
offline regression test covers this evaluator defect.

## Time and reported tokens

All six were interrupted at approximately **18 min 20 s** while still active.
Their full completion times are unknown. Neither medians of the cutoff nor the
number of tool calls provide a defensible speed ranking.

| Run | Input, including cached | Cached input subset | Output |
| --- | ---: | ---: | ---: |
| Harness 1 | 508,870 | 478,464 | 38,360 |
| Harness 2 | 899,385 | 858,624 | 37,305 |
| Harness 3 | 969,305 | 944,640 | 42,633 |
| Codex 1 | 445,411 | 419,456 | 43,592 |
| Codex 2 | 812,266 | 763,392 | 41,946 |
| Codex 3 | 684,321 | 645,888 | 42,896 |

These are **reported lower bounds**. Interrupted requests may not provide usage,
so the table must not be presented as complete billing or a total-token saving.
Across reported responses, the harness has 2,495,858 input-plus-output tokens versus
Codex's 2,070,432. Most input is cached: token-weighted cache hits are 95.97% and
94.17%. Uncached input totals are 95,832 versus 113,262; output totals are 118,298
versus 128,434. Cached input is already included in input; reasoning tokens are
already included in output.

The runtime recorded 17/30/29 harness model starts and 16/29/28 completed usage
reports. Codex recorded 12/17/17 distinct cumulative-usage updates, which are not
an exact request count. One harness run also used Python PNG decoding and ASCII
crops to recheck image characters; its local toolset lacks Codex's image-viewing
entry point. Reducing unnecessary model rounds and providing direct local image
inspection are concrete follow-up candidates. This experiment does not establish
their future speed benefit.

Reproduce with `evals.endurance.run --visual --model gpt-6.1-sol --effort xhigh
--duration 1100 --repetitions 3`, using a Release `live_driver` and the host auth
wrapper. Temporary workspaces, media, databases and raw traces are removed after
review; the source fixtures, scoring code and compact evidence remain.
