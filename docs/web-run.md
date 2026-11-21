# Batch web search

`web_run` is a built-in read tool. It sends JSON directly to Keenable's
public REST endpoint. It is available in the CLI and SDK at startup and
requires no account, API key, MCP server or local service. A search sends
the query text to Keenable. Hosts can disable the tool with
`--no-web-search` or `config.engine.search.disabled = true`.
The underlying model provider still uses its usual credentials.
[Keenable keyless access](https://docs.keenable.ai/guides/keyless-access)

The model chooses 1–100 queries and can ask for 1–10 hits per query using
`count` (default 3). Use it directly or as one child operation in a flow:

```js
const {results} = await tools.web_run({
  queries: ["SQLite WAL documentation", "libcurl multi interface overview"],
  count: 3
});
return results.map(({query, hits, error}) => ({
  query, sources: hits?.map(hit => hit.url), error
}));
```

A batch preserves input order and shares a request for exact duplicate queries.
Each hit carries a URL and the available title, snippet and publication date.
An empty hit list is a successful search. Failed queries carry an error and
attempt count. The remaining rows stay usable. Output is bounded per row and
`truncated` marks clipping. Large receipts are accessible with `read_result`.

## Transport and quotas

The client posts `query`, `max_results` and `snippet_max_length` to
`https://api.keenable.ai/v1/search/public`, with the application identifier
`X-Keenable-Title: C Harness`. Model credentials are confined to the model
transport. SDK hosts and integration tests can substitute a trusted compatible
endpoint through `search.endpoint`; HTTP loopback requires explicit permission.

All engines in the process share one admission controller. Requests start at
least 100 ms apart and can overlap as earlier transfers wait for results.
A rolling ledger includes retries in the 1,000-start hourly ceiling. The client
also observes the upstream's hourly balance and reset time, since other clients
can share the public IP. Each batch establishes its first connection before
fanning out. Rate limiting slows admission across batches, and sustained
successful responses gradually restore its pace.

Transient failures get up to two retries within the existing deadline.
`Retry-After` is respected. If the next quota window falls beyond the deadline,
pending queries receive a rate-limit error with the wait time. Cancellation
stops dispatch and keeps completed rows. The receipt's `stats` report HTTP
attempts, the batch's peak client-side in-flight transfers and HTTP 429 responses.
[Keenable rate limits](https://docs.keenable.ai/rate-limits)

## Verification

The offline contract test drives both direct model calls and `flow` against
a local HTTP fixture that checks the verb, headers, payload, result order,
deduplication, retries and cancellation. A virtual-clock test exercises both
quota boundaries without an hour-long wait.

For the live benchmark, build the CLI in Release mode and run:

```sh
python3 -B benchmarks/web_run.py ./build/ah
```

This command requests three results for each of 100 distinct public technical
queries. The model fixture issues one `flow` call and receives its result;
the CLI, QuickJS, SQLite journal and Keenable HTTPS traffic are real. The
benchmark fixes 12 canonical documentation pages before searching and reports
their first and top-three ranks. URL comparison ignores the scheme, leading
`www.`, fragment and trailing slash; versioned paths and mirrors count as misses.

On 2026-11-21 a Release build on Apple M2 completed this live batch in
16.40 seconds, with 100 nonempty result rows, 103 HTTP attempts, three
recovered 429s and 12 peak client-side in-flight transfers. Five of the
12 preselected pages ranked first and six appeared in the top three.
SQLite's WAL page and the libcurl multi overview both ranked first; Redis
SET ranked second. The PostgreSQL and CMake first hits were versioned
official documentation; the Python TaskGroup first hit was a third-party
tutorial. These are URL retrieval measurements from a single final run,
not grades of answer correctness or a latency guarantee.
