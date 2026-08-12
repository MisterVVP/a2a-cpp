# `ListTasks` JSON transport optimization

## Profile

Source-path inspection and focused wire measurements identified ProtoJSON
conversion as the dominant in-memory `ListTasks` cost. Store enumeration already
reserves the result vector and projects each task once. In contrast, the former
REST response builder and client parser each performed one typed-message to
generic-message round trip per returned task. JSON-RPC performed the same work
on both sides in addition to parsing and printing its envelope. Consequently,
one no-pagination operation with the 20-task fixture performed up to 80
per-task ProtoJSON conversions across a JSON client/server round trip.

The optimized path builds or parses one typed `ListTasksResponse` at each
transport boundary. JSON-RPC still has one unavoidable conversion between its
generic `result` envelope value and the typed response, but no longer converts
each task separately. HTTP framing, page-token handling, store enumeration, and
gRPC are unchanged.

## Focused results

The table reports medians of three equivalent runs on the same container. Each
run used 300 requests, the in-memory store, no time-based measurement window,
and the existing 20-task fixture (10 tasks for the paginated case). The latency
columns are milliseconds. All runs reported zero operation errors.

| Transport | Concurrency | Scenario | Before ops/s | After ops/s | Throughput | Before p95 | After p95 | Latency |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| HTTP+JSON | 1 | No pagination | 183.5 | 1,040.8 | +467.1% | 7.05 | 1.28 | -81.8% |
| HTTP+JSON | 16 | No pagination | 353.5 | 2,335.4 | +560.7% | 84.74 | 34.58 | -59.2% |
| HTTP+JSON | 64 | No pagination | 279.9 | 1,777.4 | +535.1% | 434.86 | 84.14 | -80.7% |
| HTTP+JSON | 1 | Pagination | 317.9 | 1,455.6 | +357.8% | 3.98 | 0.96 | -75.7% |
| HTTP+JSON | 16 | Pagination | 636.8 | 3,047.3 | +378.5% | 61.33 | 23.19 | -62.2% |
| HTTP+JSON | 64 | Pagination | 586.1 | 2,582.1 | +340.5% | 207.58 | 45.70 | -78.0% |
| JSON-RPC | 1 | No pagination | 148.7 | 231.9 | +55.9% | 8.62 | 5.85 | -32.1% |
| JSON-RPC | 16 | No pagination | 300.3 | 443.8 | +47.8% | 100.08 | 74.09 | -26.0% |
| JSON-RPC | 64 | No pagination | 280.6 | 419.6 | +49.5% | 439.13 | 340.99 | -22.3% |
| JSON-RPC | 1 | Pagination | 261.2 | 376.5 | +44.1% | 5.32 | 3.23 | -39.4% |
| JSON-RPC | 16 | Pagination | 546.1 | 763.3 | +39.8% | 63.98 | 55.46 | -13.3% |
| JSON-RPC | 64 | Pagination | 537.1 | 669.2 | +24.6% | 199.67 | 210.68 | +5.5% |
| gRPC | 64 | No pagination | 2,086.5 | 2,551.0 | +22.3% | 44.10 | 27.87 | -36.8% |
| gRPC | 64 | Pagination | 2,891.1 | 3,118.6 | +7.9% | 29.56 | 18.75 | -36.6% |

The exact command was run from both the baseline worktree and the optimized
worktree, changing only `A2A_PERF_REPORT_DIR` for each repetition:

```bash
A2A_PERF_TRANSPORTS=grpc,jsonrpc,http_json \
A2A_PERF_STORE_BACKENDS=inmemory \
A2A_PERF_REQUESTS=300 \
A2A_PERF_CONCURRENCY=1,16,64 \
A2A_PERF_SCENARIOS=ListTasks_NoPagination,ListTasks_WithPagination \
A2A_PERF_WARMUP_SECONDS=0 \
A2A_PERF_DURATION_SECONDS=0 \
A2A_PERF_REPORT_DIR=perf-{before|after}-{1|2|3} \
./scripts/run_performance_tests.sh
```

PostgreSQL comparison was not available in this container because it did not
provide a PostgreSQL server or client utilities. The in-memory results isolate
the intended encoding and wire-path changes; CI performance runs should confirm
the PostgreSQL control on a provisioned runner.
