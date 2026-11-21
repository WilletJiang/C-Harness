"""Explicit live benchmark: one native flow batch, 100 distinct public technical queries."""

import json
import sys
from pathlib import Path
from urllib.parse import urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
from test_web_run import flow, rows, run, upstream

# Predeclared URL targets for the first query in each of the first 12 topics.
# A hit requires the intended reference page, not merely the same domain.
QUERIES = {
    "SQLite": "WAL documentation; backup API; FTS5 bm25; busy timeout",
    "libcurl": "multi interface overview; curl_multi_poll; CURLOPT_COPYPOSTFIELDS; CURLINFO_TOTAL_TIME_T",
    "Python": "asyncio TaskGroup documentation; contextvars reference; tomllib loads; sqlite3 backup",
    "Git": "git worktree documentation; git range-diff; git merge-base --is-ancestor; git fetch --prune",
    "PostgreSQL": "transaction isolation documentation; EXPLAIN ANALYZE buffers; partial index predicate; VACUUM freeze",
    "Redis": "SET command reference; XRANGE command; CLIENT TRACKING; persistence AOF",
    "Go": "golang.org/x/sync/errgroup reference; context cancellation causes; io.Reader contract; race detector",
    "Rust": "std::vec::Vec reference; std pin Pin; std sync OnceLock; cargo profiles",
    "Node.js": "stream API documentation; AbortSignal.timeout; worker_threads MessageChannel; node:test mock timers",
    "Docker": "Dockerfile reference; compose healthcheck; build cache mounts; multi-stage builds",
    "Kubernetes": "Deployments documentation; PodDisruptionBudget; readiness probes; StatefulSet ordering",
    "CMake": "target_link_libraries documentation; install EXPORT; CTest fixtures; generator expressions",
    "QuickJS": "JS_ExecutePendingJob; JS_SetMemoryLimit; JS_SetInterruptHandler; JS_NewRuntime",
    "LLVM": "LangRef phi instruction; ORC JIT; AddressSanitizer; libFuzzer corpus",
    "Linux": "epoll edge triggered; io_uring_setup; pidfd_open; openat2 resolve flags",
    "OpenSSL": "EVP_DigestInit_ex; TLS session resumption; X509_VERIFY_PARAM_set1_host; BIO pairs",
    "HTTP": "RFC 9110 Retry-After; RFC 9111 caching; RFC 9113 flow control; RFC 9114 streams",
    "WebAssembly": "memory.grow instruction; WASI clocks; component model types; reference types",
    "Java": "StructuredTaskScope; CompletableFuture allOf; virtual thread scheduler; ByteBuffer slice",
    "TypeScript": "satisfies operator; conditional types infer; mapped types modifiers; project references",
    "Swift": "TaskGroup concurrency; actor isolation; package manifest products; AsyncStream buffering",
    "NumPy": "broadcasting rules; einsum path; as_strided; array memory order",
    "Nginx": "proxy_read_timeout; limit_req_zone; upstream keepalive; proxy_buffering",
    "Prometheus": "histogram_quantile; rate counter resets; recording rules; relabel_configs",
    "C17": "atomic_compare_exchange; memory_order_acquire; aligned_alloc; thread local storage duration",
}

REFERENCE_PAGES = {
    "SQLite": "sqlite.org/wal.html",
    "libcurl": "curl.se/libcurl/c/libcurl-multi.html",
    "Python": "docs.python.org/3/library/asyncio-task.html",
    "Git": "git-scm.com/docs/git-worktree",
    "PostgreSQL": "postgresql.org/docs/current/transaction-iso.html",
    "Redis": "redis.io/docs/latest/commands/set",
    "Go": "pkg.go.dev/golang.org/x/sync/errgroup",
    "Rust": "doc.rust-lang.org/std/vec/struct.Vec.html",
    "Node.js": "nodejs.org/api/stream.html",
    "Docker": "docs.docker.com/reference/dockerfile",
    "Kubernetes": "kubernetes.io/docs/concepts/workloads/controllers/deployment",
    "CMake": "cmake.org/cmake/help/latest/command/target_link_libraries.html",
}


def normalized_url(url):
    parsed = urlsplit(url if "://" in url else "https://" + url)
    return (parsed.hostname or "").lower().removeprefix("www.") + parsed.path.rstrip(
        "/"
    )


def main(cli):
    queries = [
        f"{topic} {term}"
        for topic, terms in QUERIES.items()
        for term in terms.split("; ")
    ]
    assert len(set(queries)) == len(queries) == 100
    with upstream() as fixture:
        elapsed, ops = run(cli, fixture, flow(queries, 3), live=True, timeout=90000)
        results = rows(fixture)
        assert [r["query"] for r in results] == queries
        assert ops == [("flow", "done"), ("web_run", "done")]
        ranks = {}
        first_hits = {}
        for i, topic in enumerate(QUERIES):
            if topic in REFERENCE_PAGES:
                urls = [
                    normalized_url(hit["url"]) for hit in results[i * 4].get("hits", [])
                ]
                first_hits[topic] = urls[0] if urls else None
                ranks[topic] = next(
                    (
                        k
                        for k, url in enumerate(urls, 1)
                        if url == normalized_url(REFERENCE_PAGES[topic])
                    ),
                    None,
                )
        print(
            json.dumps(
                {
                    "elapsed_s": round(elapsed, 3),
                    **fixture.result["result"]["stats"],
                    "nonempty": sum(bool(r.get("hits")) for r in results),
                    "errors": [r for r in results if "error" in r],
                    "exact_top1": sum(v == 1 for v in ranks.values()),
                    "exact_top3": sum(v is not None for v in ranks.values()),
                    "ranks": ranks,
                    "first_hits": first_hits,
                },
                indent=2,
            )
        )


if __name__ == "__main__":
    main(sys.argv[1])
