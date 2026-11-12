# A2A performance driver

`a2a_performance_driver` is the in-process C++ SDK performance driver used by
`scripts/run_performance_tests.sh` and `scripts/performance_runner.py`. It runs a
fixed set of report-only scenarios against the SDK's example executor and prints
a JSON array of per-scenario measurements.

## What it measures

The driver covers task, streaming, subscription, push-configuration, and push
notification paths. Each scenario reports:

- operation counts and success/error totals;
- throughput in operations per second;
- latency percentiles (`p50`, `p90`, `p95`, `p99`) and maximum latency in
  milliseconds;
- metadata identifying the transport label, store backend, and SDK dispatch
  path.

Warmup operations run before timing starts, so setup and warmup work are excluded
from performance calculations. During measured execution, each worker records its
own counters and latency samples, then the driver aggregates those samples after
workers finish. This avoids adding result-collection mutex contention to the
measured operation latency.

The historical wire streaming scenario names use one `A2AClient` per benchmark
worker, preserving the topology used by earlier reports. HTTP+JSON and JSON-RPC
also expose `SendStreamingMessage_FiniteStream_SharedClient` and
`SubscribeToTask_FirstEventLatency_SharedClient`. Those variants route every
measured worker through one `A2AClient` without changing the historical
comparison rows. HTTP streaming easy handles are sharded across a process-wide
pool of at most four libcurl reactors, while callbacks use the fixed shared
dispatch pool. Reusable stream slots retain their reactor assignment so
sequential finite streams keep connection-cache locality. gRPC does not run the
shared-client variants. HTTP streaming rows include `client_process_thread_count`,
allowing thread growth to be compared across concurrency levels without
conflating the two client topologies.

HTTP wire rows also report finite-stream connection lifecycle diagnostics. The
`http_finite_stream_connections` and `http_completed_finite_streams` fields
count connections carrying finite streams and successfully completed finite
streams. `http_finite_streams_per_connection` reports their ratio, while
`http_connections_reused_after_finite_stream` counts successful parsing of a
subsequent request on a connection after its terminal chunk. The counters are
collected for the complete SUT run and are therefore shared by all scenario rows
from that run.

### Follow-up workloads

Follow-up fixtures are fully created before the measured region. Each measured
operation uses a distinct task, so request count and execution order do not alter
the task's initial state:

- `SendMessage_FollowUpExistingTask` starts with one history entry and measures
  one follow-up, representing the common first-follow-up case.
- `SendMessage_FollowUpAtHistoryDepth/8` starts with exactly eight history
  entries and measures one follow-up, explicitly exercising deeper-history
  processing without continually growing a shared hot task.

Both the in-process and wire drivers use these semantics. Their JSON and CSV rows
include the numeric `history_depth` field in addition to the depth-bearing stress
scenario name, keeping the workload distinction machine-readable.

## Store backends

By default, scenarios use the in-memory stores owned by the example executor. To
exercise PostgreSQL-backed stores, run with `--store-backend postgres` and set
`A2A_TEST_POSTGRES_DSN` to a valid connection string. The driver creates a unique
schema name for each PostgreSQL run.

## Example

```bash
./build/tests/a2a_performance_driver \
  --transport grpc \
  --store-backend inmemory \
  --requests 1000 \
  --concurrency 4 \
  --warmup-seconds 1
```

For the canonical wrapper and generated summary artifacts, prefer:

```bash
./scripts/run_performance_tests.sh --store-backends inmemory --requests 1000 --concurrency 1,4
```

### Compare synchronous and async servers

With the optional Boost.Asio dependencies installed (see the
[installation guide](../getting-started/installation.md)), run the same driver against both server modes:

```bash
./scripts/run_performance_tests.sh --server-modes sync,async \
  --store-backends inmemory --transports grpc,http_json,jsonrpc \
  --requests 1000 --concurrency 1,4,16,64 --repetitions 3 \
  --report-dir perf-artifacts/normal
```

The runner configures Release with `A2A_ENABLE_ASIO=ON` and uses one
`performance_sut` binary for both modes. Async mode selects the native async gRPC
service and `EventHttpServer`/SSE handler; startup must confirm the requested
mode. Both use identical fixtures and the synchronous wire client by default.
The in-memory fixture runs ready operations on the coroutine executor and live
subscriptions await native publication, labeled `native_coroutine`. Stores that
need blocking work use `blocking_adapter`; PostgreSQL async measurements are
outside the initial matrix.

Add `--client-modes sync,async` (`A2A_PERF_CLIENT_MODES`) to measure all four
client/server combinations for `GetTask_ExistingTask`, `ListTasks_NoPagination`,
and `ListTasks_WithPagination`. Async client lanes submit the next request from
the completion callback, with the same concurrency limit as sync workers;
they do not block one thread per request. Other workloads retain their existing
client API. Streaming has one observer API and is measured once per server mode.
PostgreSQL remains sync-only.

For a sustained comparison of read-only calls and finite streams:

```bash
./scripts/run_performance_tests.sh --server-modes sync,async --client-modes sync,async \
  --store-backends inmemory --transports grpc,http_json,jsonrpc \
  --scenarios GetTask_ExistingTask,ListTasks_WithPagination,SendStreamingMessage_FiniteStream \
  --requests 1000000 --duration-seconds 3 --warmup-seconds 0.25 \
  --concurrency 1,64 --repetitions 3 --report-dir perf-artifacts/normal/sustained
```

Each wire run warms the actual measured clients through the selected API and
retains their connections. Read-only sync/async client cases share the same
fixtures and warmup concurrency. Both duration and request count are limits:
use a generous request cap for a timed run. Comparison validation rejects runs
that exhaust that cap before reaching the requested duration. Follow-up and
delete workloads still require distinct preseeded fixtures per request; keep
their request counts modest instead of using this sustained-load command.

`--stream-hold-ms 100` (`A2A_PERF_STREAM_HOLD_MS`) holds established subscriptions
in `IdleStream_ClientCancellationLatency` before cancelling them. This exercises
waiting I/O at the selected concurrency, while reported cancellation latency
still excludes the hold. The value is recorded in reports. The sustained CI
step includes this workload as well as read-only calls and finite streams.

The async matrix covers all supported in-memory wire workloads, including unary
operations, push configuration, finite streams, subscriptions, and cancellation.
Use `--scenarios` to narrow a local run. PostgreSQL retains synchronous coverage
and shows `n/a` for async; its storage is unchanged. In-process results remain
separate because they bypass the gRPC/HTTP server transports.
`--server-modes sync` preserves the default suite; the corresponding
environment variables are `A2A_PERF_SERVER_MODES` and `A2A_PERF_REPETITIONS`.

Comparison runs default to three repetitions, alternate server order, and start
a fresh SUT for each scenario/mode. On Linux, automatic CPU placement divides
the available CPU budget between driver and server, taking the cgroup v2 quota
into account and giving the driver the extra CPU for odd budgets. Both server modes receive the
same CPU set. Reports record placement and quota; fewer than two CPUs and
platforms without affinity support share CPU capacity. `--cpu-placement shared`
disables separation. Check this metadata before interpreting results on a
shared or throttled machine.

When supplying `A2A_PERF_DRIVER`, `A2A_PERF_WIRE_DRIVER`, or `A2A_PERF_SUT`, build
all three from the same revision with Release and matching optional dependencies.
The runner cannot verify the provenance of external binaries.

## PostgreSQL tail validation

Run the focused tail-validation matrix against a local PostgreSQL instance with:

```bash
A2A_TEST_POSTGRES_DSN=postgresql://a2a:a2a@127.0.0.1:5432/a2a \
  ./scripts/run_performance_tests.sh --profile postgres-tail \
  --report-dir perf-artifacts/postgres-tail
```

The command runs five focused in-process scenarios five times for every
combination of concurrency `4,16,64` and PostgreSQL pool size `4,16,64`, using
2,000 requests and a one-second warmup. It writes per-run and median aggregate
JSON, CSV, and Markdown reports under
`perf-artifacts/postgres-tail/`. The report also records `EXPLAIN (ANALYZE, BUFFERS)`
output and checks the task lookup and the explicit push-config `created_sequence`
sort. The removed push-config ordering index must not appear in the combined plan.
