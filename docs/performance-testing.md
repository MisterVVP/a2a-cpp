# Performance testing

The A2A C++ SDK includes a report-only performance test kit for repeatable local
and CI measurements across TCK-aligned SDK operation paths. The first version is
intended to produce consistent artifacts without introducing threshold gates;
future revisions can replace individual in-process drivers with deeper
wire-level probes and baseline comparisons.

## Run locally

```bash
./scripts/run_performance_tests.sh
```

The runner writes:

- `perf-artifacts/results.json`
- `perf-artifacts/results.csv`
- `perf-artifacts/summary.md`

## Configuration

You can configure the matrix and load profile with command-line flags or the
matching environment variables.

| Setting | Environment variable | Default |
| --- | --- | --- |
| Transports (`grpc`, `jsonrpc`, `http_json`, or `all`) | `A2A_PERF_TRANSPORTS` | `grpc,jsonrpc,http_json` |
| Store backends (`inmemory`, `postgres`, or `all`) | `A2A_PERF_STORE_BACKENDS` | `inmemory,postgres` |
| Operations per result row | `A2A_PERF_REQUESTS` | `1000` |
| Concurrency levels | `A2A_PERF_CONCURRENCY` | `1,4` |
| Warmup seconds | `A2A_PERF_WARMUP_SECONDS` | `1` |
| Duration seconds metadata | `A2A_PERF_DURATION_SECONDS` | `0` |
| Report directory | `A2A_PERF_REPORT_DIR` | `perf-artifacts` |

Example smoke run:

```bash
A2A_PERF_TRANSPORTS=grpc \
A2A_PERF_STORE_BACKENDS=inmemory \
A2A_PERF_REQUESTS=100 \
A2A_PERF_CONCURRENCY=1,4 \
A2A_PERF_WARMUP_SECONDS=0 \
./scripts/run_performance_tests.sh
```

## Scenario coverage

The suite emits one result object for each scenario, transport, store backend,
and concurrency combination. Scenarios are aligned with TCK-relevant behavior:

- task lifecycle: send, get, cancel, list, follow-up send, and missing-task error;
- streaming and subscriptions: finite streaming send, first-event latency,
  multiple subscribers, terminal completion, and subscriber disconnect;
- push notifications: configuration create/get/list/delete, many-config update
  notification, and fake local callback delivery latency;
- transport labels: gRPC, JSON-RPC, and HTTP+JSON;
- store labels: in-memory and PostgreSQL-backed task/push notification stores.

## Report-only CI behavior

The CI performance job runs a larger report-only matrix with 2,500 operations per
result row, appends `summary.md` to the GitHub Actions step summary, and uploads
the `perf-artifacts` directory. The summary starts with a scenario rollup table
so reviewers can quickly compare
operations, success/error counts, throughput, and worst observed latency before
opening the full detailed matrix results. It fails only if the runner crashes,
reports operation errors, or fails to produce valid report files. It does not
compare against latency or throughput thresholds yet.

## k6 consideration

k6 is a strong candidate for future wire-level HTTP+JSON and JSON-RPC load
testing because it has mature virtual-user orchestration and CI-friendly
summaries. The first version intentionally keeps the runner in Python so it has
no new runtime dependency, can cover non-HTTP transport/store labels in one
matrix, and remains deterministic in local and CI smoke runs. When we add real
network probes, k6 can be introduced alongside this runner for HTTP-based
scenarios while gRPC and store-specific scenarios continue to use SDK-native
drivers.

## JSON shape

`results.json` contains host metadata and a `results` array. Each result includes
scenario name, transport, store backend, concurrency, operation counts,
throughput, warmup-adjusted latency percentiles, max latency, SDK commit SHA in
metadata, and host OS/CPU metadata.
