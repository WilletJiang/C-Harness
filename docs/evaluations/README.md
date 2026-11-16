# Evaluation record index

These are historical measurements, not current behavior specifications. The JSON
ledgers retain compact metrics and failures; raw generated workspaces were removed.
Source hashes and old command names refer to the commits recorded in each report.
The current entrypoints are documented in [evals](../../evals/README.md).

| Record | Question |
| --- | --- |
| [Local performance](local-performance.md) | Local orchestration, file operations and read batching |
| [All nine tools](tool-performance.md) | Paired local timings, real file batching, large edits, paging, recovery and output checks |
| [Terra endurance](endurance-evaluation.md) | Three concurrent paired long tasks before the cache fix |
| [Cache routing](cache-evaluation.md) | JSON key versus stable HTTP session identity |
| [Sol endurance](sol-xhigh-evaluation.md) | Long tasks with cache routing and streamed-item fixes |
| [Image-based Sol endurance](vision-sol-xhigh.md) | Three concurrent image-grounded runs per runner; quality and censored token accounting |
| [WebSocket continuation](websocket-evaluation.md) | Pooled HTTP versus full/incremental WS, subscription recovery and all attempt usage |
| [Workflow profile](execution-evaluation.md) | Old/new native tools on unchanged smoke tasks; quality, wall time, usage and phase measurements |
| [Native find](find-performance.md) | Exact-output native search optimization, callback timings and tool-interface consolidation |
| [Durable runtime](durable-runtime.md) | Ordinary execution overhead and parked-worker resources with per-signal and batched host ingress |

The frozen prompt and specification survived the package reorganization unchanged.
The grader assertions are also unchanged; package imports and their source-file
hashes differ. Use the historical commits for byte-identical reproduction.
