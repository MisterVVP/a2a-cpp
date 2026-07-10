# Performance testing

The A2A C++ SDK includes a report-only performance test kit for repeatable local
and CI measurements. The Python runner still owns matrix orchestration and writes
`results.json`, `results.csv`, and `summary.md`; measured operations are delegated
to the in-process C++ SDK-backed driver (`a2a_performance_driver`) and, for real transport rows, the wire-level client driver (`a2a_wire_performance_driver`).

## Run locally

```bash
./scripts/run_performance_tests.sh --store-backends inmemory --requests 100 --concurrency 1,4 --warmup-seconds 0
```

The runner writes:

- `perf-artifacts/results.json`
- `perf-artifacts/results.csv`
- `perf-artifacts/summary.md`

If `A2A_PERF_DRIVER`, `A2A_PERF_WIRE_DRIVER`, or `A2A_TCK_SUT` are not set, the runner configures and builds the needed binaries in `build/performance`. Set `A2A_PERF_BUILD_DIR` to reuse another CMake build tree.

## Configuration

You can configure the matrix and load profile with command-line flags or the
matching environment variables.

| Setting | Environment variable | Default |
| --- | --- | --- |
| Transports (`grpc`, `jsonrpc`, `http_json`, or `all`) | `A2A_PERF_TRANSPORTS` | `grpc,jsonrpc,http_json` |
| Store backends (`inmemory`, `postgres`, or `all`) | `A2A_PERF_STORE_BACKENDS` | `inmemory,postgres` |
| Operations per result row | `A2A_PERF_REQUESTS` | `2000` |
| Concurrency levels | `A2A_PERF_CONCURRENCY` | `1,4` |
| Warmup seconds | `A2A_PERF_WARMUP_SECONDS` | `1` |
| Duration seconds metadata | `A2A_PERF_DURATION_SECONDS` | `0` |
| Report directory | `A2A_PERF_REPORT_DIR` | `perf-artifacts` |
| Existing in-process driver binary | `A2A_PERF_DRIVER` | unset |
| Existing wire driver binary | `A2A_PERF_WIRE_DRIVER` | unset |
| Existing TCK SUT binary | `A2A_TCK_SUT` | unset |
| In-process driver timeout seconds | `A2A_PERF_DRIVER_TIMEOUT_SECONDS` | `600` |
| Wire driver timeout seconds | `A2A_PERF_WIRE_DRIVER_TIMEOUT_SECONDS` | `600` |
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

Reports contain two clearly separated measurement paths:

- In-process rows come from `a2a_performance_driver`. They exercise SDK service,
  executor, store, streaming, and push-notification code without sockets and are
  reported as `driver_type=cpp_sdk_in_process` with `transport_path=in_process`.
- Wire rows come from `a2a_wire_performance_driver`. The runner starts the shared
  `tck_sut` fixture, waits for HTTP and gRPC ports, and then the wire driver sends
  real client calls to the selected endpoint. These rows are reported as
  `driver_type=wire_tck_sut` with `transport_path=wire_http_json`,
  `wire_jsonrpc`, or `wire_grpc`.

The current real wire-level scenario set covers core lifecycle operations for
HTTP+JSON, JSON-RPC, and gRPC: `ListTasks_NoPagination`,
`ListTasks_WithPagination`, `SendMessage_CreateTask`, `GetTask_ExistingTask`,
`CancelTask_WorkingTask`, `SendMessage_FollowUpExistingTask`, and
`GetTask_MissingTaskError`. The wire driver reuses one client/transport per
worker thread so measured operations do not recreate gRPC channels or HTTP
transport objects. The libcurl-backed HTTP client also keeps a reusable easy
handle per SDK HTTP client, avoiding repeated easy-handle setup on REST and
JSON-RPC paths. List scenarios run before mutating lifecycle scenarios and
seed a fixed fixture of 20 tasks, then measure only `ListTasks` calls, keeping
the listed task set bounded in CI. Streaming/subscription and push notification
scenarios remain in-process-only until dedicated wire clients are added for
those flows; they must not be interpreted as `wire_tck_sut` coverage.

## Store backend coverage

`inmemory` uses the real in-memory task and push notification stores. `postgres`
uses the repository `PostgresStoreFactory` to create real PostgreSQL-backed task
and push notification stores when the driver is built with
`A2A_ENABLE_POSTGRES_STORE=ON`. The Python runner automatically adds that CMake
option when `postgres` is selected and it needs to auto-build the driver.
PostgreSQL runs must provide the same local DSN style used by the repository
store tests (`A2A_TEST_POSTGRES_DSN`); CI starts a local PostgreSQL service for
the performance job. For wire-level PostgreSQL rows, the runner maps
`A2A_TEST_POSTGRES_DSN` to `A2A_TCK_POSTGRES_DSN` for `tck_sut` and assigns a
matrix-scoped schema named `a2a_perf_<transport>_<concurrency>_<port>` so rows
do not share the default `public` schema or accumulate data across matrix
entries.

## CI behavior

The performance job remains report-only and keeps the current CI wire matrix of
three transports, two stores, 2,000 operations, and concurrency levels 1 and 4.
The in-process SDK/service/store rows do not exercise a transport, so the runner
executes them once per store/concurrency pair instead of repeating identical
in-process work under every selected transport. It uploads `perf-artifacts`,
appends `summary.md` to the GitHub Actions step
summary, and fails only on crashes, functional operation errors, malformed
output, missing artifacts, or driver timeouts. It does not enforce latency or
throughput thresholds. The runner prints a workload estimate at startup and
flushes `[perf] start ...` / `[perf] done ...` progress lines for every
in-process and wire matrix row so GitHub Actions logs show forward progress.
Both driver subprocesses have explicit timeouts controlled by
`A2A_PERF_DRIVER_TIMEOUT_SECONDS` and
`A2A_PERF_WIRE_DRIVER_TIMEOUT_SECONDS`; on a wire timeout, recent `tck_sut` logs
are included in the failure message when available.

## Larger local benchmark

```bash
A2A_PERF_TRANSPORTS=grpc,jsonrpc,http_json \
A2A_PERF_STORE_BACKENDS=inmemory,postgres \
A2A_PERF_REQUESTS=2000 \
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

## Shared TCK SUT wire-level driver

The `tck_sut` binary is shared infrastructure for TCK conformance and
wire-level performance coverage. It is built by the performance runner when
needed, started on a local test port, checked for HTTP and gRPC readiness, and
stopped cleanly after each wire-level matrix entry. Startup logs are captured in
the report directory as `tck_sut_<store>_<port>.log` for CI diagnosis.

Endpoint layout is the same as the TCK flow:

* REST/HTTP+JSON: `http://<host>:<port>/a2a`
* JSON-RPC: `http://<host>:<port>/rpc`
* gRPC: `<host>:<port + 1>`

The SUT supports `A2A_TCK_STORE_BACKEND=inmemory|postgres`,
`A2A_TCK_POSTGRES_DSN`, `A2A_TCK_POSTGRES_SCHEMA`, and the existing extended
agent-card mode environment variable. Run it manually with:

```bash
cmake --build build-tck --target tck_sut
./build-tck/tests/tck_sut 127.0.0.1:50061
```

Performance reports distinguish the low-overhead SDK service/store layer from
transport-level coverage. In-process rows use
`driver_type=cpp_sdk_in_process` and `transport_path=in_process`. Wire rows use
`driver_type=wire_tck_sut` and one of `wire_http_json`, `wire_jsonrpc`, or
`wire_grpc`. Initial wire coverage is the core lifecycle set: bounded list with
and without pagination, send/create, get existing, cancel working, follow-up
send, and missing-task get errors. Streaming and push-notification rows remain
in-process until dedicated wire clients are added.
