# Performance testing

The A2A C++ SDK includes a report-only performance test kit for repeatable local
and CI measurements. The Python runner still owns matrix orchestration and writes
`results.json`, `results.csv`, and `summary.md`; measured operations are delegated
to the C++ SDK-backed driver built as `a2a_performance_driver`.

## Run locally

```bash
./scripts/run_performance_tests.sh --store-backends inmemory --requests 100 --concurrency 1,4 --warmup-seconds 0
```

The runner writes:

- `perf-artifacts/results.json`
- `perf-artifacts/results.csv`
- `perf-artifacts/summary.md`

If `A2A_PERF_DRIVER` is not set, the runner configures and builds the driver in
`build/performance`. Set `A2A_PERF_BUILD_DIR` to reuse another CMake build tree.

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
| Existing driver binary | `A2A_PERF_DRIVER` | unset |
| Auto-build directory | `A2A_PERF_BUILD_DIR` | `build/performance` |

## Scenario coverage

The SDK-backed driver executes these real service/store flows through the shared
example executor, task lifecycle service, task subscription service, in-memory
task store, in-memory push notification store, and push notification service:

- task lifecycle: `SendMessage` create, `GetTask`, `CancelTask`, list with and
  without pagination, follow-up `SendMessage`, and missing-task lookup;
- streaming and subscriptions: finite `SendStreamingMessage`, existing-task
  subscription first event, multi-subscriber reads, terminal stream completion,
  and a disconnect-style subscriber read where other subscriptions still run;
- push notifications: create/get/list/delete config, notify many configs for one
  task update, and delivery callback latency through a local recording delivery
  client implementation of the SDK delivery interface.

Each row includes the required stable fields plus `driver_type` and
`transport_path`. The current driver type is `cpp_sdk_in_process`.

## Transport coverage

The current C++ driver is SDK-backed and in-process. It validates the selected
transport and records a transport-specific `transport_path` (`sdk_grpc_server_dispatch`,
`sdk_jsonrpc_server_dispatch`, or `sdk_http_json_server_dispatch`) so reports stay
compatible with the existing transport matrix. It does not yet open sockets or
run external clients; k6 is not required. Future work can replace individual
transport paths with wire-level gRPC, JSON-RPC, and HTTP+JSON probes without
changing the report format.

## Store backend coverage

`inmemory` uses the real in-memory task and push notification stores. `postgres`
uses the repository `PostgresStoreFactory` to create real PostgreSQL-backed task
and push notification stores when the driver is built with
`A2A_ENABLE_POSTGRES_STORE=ON`. The Python runner automatically adds that CMake
option when `postgres` is selected and it needs to auto-build the driver.
PostgreSQL runs must provide the same local DSN style used by the repository
store tests (`A2A_TEST_POSTGRES_DSN`); CI starts a local PostgreSQL service for
the performance job.

## CI behavior

The performance job remains report-only: it uploads `perf-artifacts`, appends
`summary.md` to the GitHub Actions step summary, and fails only on crashes,
functional operation errors, malformed output, or missing artifacts. It does not
enforce latency or throughput thresholds.

## Larger local benchmark

```bash
A2A_PERF_TRANSPORTS=grpc,jsonrpc,http_json \
A2A_PERF_STORE_BACKENDS=inmemory,postgres \
A2A_PERF_REQUESTS=1000 \
A2A_PERF_CONCURRENCY=1,4,16,64 \
A2A_PERF_WARMUP_SECONDS=5 \
A2A_PERF_REPORT_DIR=perf-artifacts \
./scripts/run_performance_tests.sh
```

## JSON shape

`results.json` contains host metadata and a `results` array. Each result includes
scenario name, transport, store backend, concurrency, operation counts, success
and error counts, throughput, latency percentiles, max latency, SDK commit SHA in
metadata, and host OS/CPU metadata.
