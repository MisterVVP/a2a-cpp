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

### Reading the reports

The Markdown summary keeps fundamentally different workloads separate. Its
execution summary groups counts by store and measurement path for compact
coverage and correctness checking, but deliberately contains no combined
throughput or latency statistic. Subsequent tables separate in-memory from
PostgreSQL, in-process from wire measurements, and each wire transport from the
others. PostgreSQL pool sizes also have distinct tables. This prevents a faster
in-memory row, a wire round trip, or a different pool configuration from being
averaged into a number that describes no concrete workload.

Each result table pivots the configured concurrency levels into `cN ops/s` and
`cN p95 ms` column pairs. Read horizontally to see how one scenario at one
store/path/transport/pool coordinate scales; columns are generated from the
levels actually present, including single-level runs. In-process rows exercise
SDK service and store code without a transport, so Markdown renders their
transport as `—` rather than attributing them to the transport selected to
launch the driver. These coordinate tables are collapsed by default so the
execution summary remains easy to scan; expand the named section to inspect its
scaling results.

The **Cross-backend scaling signals** table uses only scenario, path, transport,
and concurrency coordinates present in both stores, with each PostgreSQL pool
kept separate. For each coordinate it divides the highest shared-concurrency
p95 and throughput by their respective lowest shared-concurrency values. A zero
denominator is reported as `n/a`. These relative ratios help expose scaling
patterns shared by both backends; they do not compare absolute PostgreSQL speed
with in-memory speed and are not an automatic bottleneck verdict.

The detailed matrix remains available in a collapsible section. Its
`Repetition` column appears only when at least one row belongs to a repeated
profile, such as `postgres-tail`. `results.csv` and `results.json` retain every
raw row and field and are the authoritative outputs for further analysis; the
Markdown restructuring does not transform those measurements.
The cross-backend signals, PostgreSQL diagnostics, and `postgres-tail` median
tables are also collapsible because they can contain many rows.

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
| Duration seconds limit | `A2A_PERF_DURATION_SECONDS` | `0` |
| Report directory | `A2A_PERF_REPORT_DIR` | `perf-artifacts` |
| Existing in-process driver binary | `A2A_PERF_DRIVER` | unset |
| Existing wire driver binary | `A2A_PERF_WIRE_DRIVER` | unset |
| Existing TCK SUT binary | `A2A_TCK_SUT` | unset |
| In-process driver timeout seconds | `A2A_PERF_DRIVER_TIMEOUT_SECONDS` | `600` |
| Wire driver timeout seconds | `A2A_PERF_WIRE_DRIVER_TIMEOUT_SECONDS` | `600` |
| Auto-build directory | `A2A_PERF_BUILD_DIR` | `build/performance` |
| PostgreSQL pool sizes | `A2A_PERF_POSTGRES_POOL_SIZES` | `4` |

## Workload modes

When `A2A_PERF_DURATION_SECONDS=0`, each row runs request-count mode and stops after `A2A_PERF_REQUESTS` operations. When `A2A_PERF_DURATION_SECONDS` is greater than zero, each row stops when either the configured request count or the duration limit is reached, whichever happens first. Warmup runs before the measured window, so warmup operations are excluded from latency, throughput, `operations`, and `measured_duration_seconds`. Reports include both `configured_requests` / `configured_duration_seconds` and the actual `operations` / `measured_duration_seconds`.

## Scenario coverage

The SDK-backed driver executes these real service/store flows through the shared
example executor, task lifecycle service, task subscription service, in-memory
task store, in-memory push notification store, and push notification service:

- task lifecycle: `SendMessage` create, `GetTask`, `CancelTask`, list with and
  without pagination, follow-up `SendMessage`, and missing-task lookup;
- streaming and subscriptions: finite `SendStreamingMessage`, existing-task
  subscription first event, simultaneous multi-subscriber delivery to the same task, terminal update delivery followed by stream completion,
  and disconnect isolation where one cancelled subscription does not prevent remaining subscribers from receiving a later update;
- push notifications: create/get/list/delete config, an end-to-end many-config task update, pre-seeded many-config list lookup, many-config create cost, payload construction, and in-process callback fan-out through a local recording delivery client implementation of the SDK delivery interface. Rows expose scenario-specific counters such as `successful_deliveries`, `failed_deliveries`, `event_count`, `callback_count`, `fanout_per_operation`, and `total_fanout_count`.

Each row includes the required stable fields plus `driver_type` and
`transport_path`. The current driver type is `cpp_sdk_in_process`.

### Push notification row attribution

The in-process push rows intentionally separate setup-heavy and delivery-only work:

- `PushNotify_EndToEndManyConfigs` creates a task, writes eight push-notification configs, sends one task update, lets the push service list configs from the configured store, builds the update payload, and invokes the local recording callback once per config.
- `PushConfig_ListManyConfigs` uses a task and eight configs seeded before warmup and measures only the list operation for that fixed fan-out.
- `PushDelivery_CallbackFanout` uses preloaded configs and a prebuilt payload seeded before warmup, then measures only the in-process callback delivery loop. It does not create tasks, create configs, query the store, or perform network I/O inside the timed operation.
- `PushConfig_CreateMany` uses a task seeded before warmup and measures only eight real `CreateTaskPushNotificationConfig` calls, including the production task-existence lookup for each config creation.
- `PushDelivery_BuildPayload` measures construction of the push status-update payload separately.

The default fan-out is eight configs per operation and is reported in `fanout_per_operation`. `total_fanout_count` is cumulative across measured operations and reflects actual attempted/configured fan-out where known; for example, 2,000 successful operations at fan-out 8 report `fanout_per_operation=8` and `total_fanout_count=16000`. `fanout_count` remains as a backward-compatible alias for the cumulative total. Callback rows report actual attempted callbacks in `callback_count` and split delivery outcomes into `successful_deliveries` and `failed_deliveries`, including failed operations.

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

The current real wire-level scenario set covers core lifecycle operations, push notification config CRUD, finite streaming, and first-event task subscription for HTTP+JSON, JSON-RPC, and gRPC. The common wire scenarios are `ListTasks_NoPagination`,
`ListTasks_WithPagination`, `SendMessage_CreateTask`, `GetTask_ExistingTask`,
`CancelTask_WorkingTask`, `SendMessage_FollowUpExistingTask`,
`SendMessage_FollowUpAtHistoryDepth/8`,
`GetTask_MissingTaskError`, `PushConfig_Create`, `PushConfig_Get`,
`PushConfig_List`, `PushConfig_Delete`, `SendStreamingMessage_FiniteStream`, and `SubscribeToTask_FirstEventLatency`. The wire driver reuses one client/transport per
worker thread so measured operations do not recreate gRPC channels or HTTP
transport objects. The libcurl-backed HTTP client also keeps a reusable easy
handle per SDK HTTP client, avoiding repeated easy-handle setup on REST and
JSON-RPC paths. List scenarios run before mutating lifecycle scenarios and
seed a fixed fixture of 20 tasks, then measure only `ListTasks` calls, keeping
the listed task set bounded in CI. Multi-subscriber subscription, disconnect isolation, terminal-completion subscription, and callback fan-out remain SDK in-process rows in this implementation; they are not duplicated as transport rows and must not be interpreted as full `wire_tck_sut` coverage.

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
The runner passes each selected size to both the in-process driver and wire SUT.
Every PostgreSQL result row records `postgres_pool_size` in JSON and CSV, and
the Markdown detail and diagnostic tables include a pool-size column. Multiple
comma-separated sizes create a comparison matrix without source changes. The
`postgres-tail` profile defaults to pool sizes `4,16,64` at concurrency levels
`4,16,64`; an explicit `--postgres-pool-sizes` value or
`A2A_PERF_POSTGRES_POOL_SIZES` overrides that profile default.

## CI behavior

CI runs the broad performance matrix and focused PostgreSQL tail profile as
independent jobs so they execute in parallel. The normal job uses 1,000 requests
per row across three transports, two stores, and concurrency levels 1, 4, 16,
and 64; its PostgreSQL rows use a 64-connection pool. The tail job retains 2,000
requests, five repetitions, pool sizes `4,16,64`, and concurrency levels
`4,16,64`.

The in-process SDK/service/store rows do not exercise a transport, so the runner
executes them once per store/concurrency pair instead of repeating identical
in-process work under every selected transport. The jobs upload
`perf-artifacts-normal` and `perf-artifacts-postgres-tail`, append their own
summaries to the GitHub Actions step summary, and fail only on crashes,
functional operation errors, malformed output, missing artifacts, or driver
timeouts. They do not enforce latency or throughput thresholds. The runner
prints a workload estimate at startup and flushes `[perf] start ...` /
`[perf] done ...` progress lines for every in-process and wire matrix row so
GitHub Actions logs show forward progress. Both driver subprocesses have
explicit timeouts controlled by `A2A_PERF_DRIVER_TIMEOUT_SECONDS` and
`A2A_PERF_WIRE_DRIVER_TIMEOUT_SECONDS`; on a wire timeout, recent `tck_sut` logs
are included in the failure message when available.

## Larger local benchmark

```bash
A2A_PERF_TRANSPORTS=grpc,jsonrpc,http_json \
A2A_PERF_STORE_BACKENDS=inmemory,postgres \
A2A_PERF_REQUESTS=2000 \
A2A_PERF_POSTGRES_POOL_SIZES=4,16,64 \
A2A_PERF_CONCURRENCY=1,4,16,64 \
A2A_PERF_WARMUP_SECONDS=5 \
A2A_PERF_REPORT_DIR=perf-artifacts \
./scripts/run_performance_tests.sh
```

## JSON shape

`results.json` contains host metadata and a `results` array. Each result includes
scenario name, transport, store backend, driver type, transport path, concurrency, configured request and duration limits, measured duration, operation counts, success and error counts, throughput, latency percentiles, max latency, scenario-specific delivery/event/callback counters, SDK commit SHA in metadata, and host OS/CPU metadata.

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
`A2A_TCK_POSTGRES_DSN`, `A2A_TCK_POSTGRES_SCHEMA`,
`A2A_TCK_POSTGRES_POOL_SIZE` (default `4`), and the existing extended
agent-card mode environment variable. The TCK conformance workflow explicitly
starts the PostgreSQL-backed SUT with a pool size of `64`, while the SDK-facing
default remains `4` for compatibility. Run it manually with:

```bash
cmake --build build-tck --target tck_sut
./build-tck/tests/tck_sut 127.0.0.1:50061
```

Performance reports distinguish the low-overhead SDK service/store layer from
transport-level coverage. In-process rows use
`driver_type=cpp_sdk_in_process` and `transport_path=in_process`. Wire rows use
`driver_type=wire_tck_sut` and one of `wire_http_json`, `wire_jsonrpc`, or
`wire_grpc`. Wire coverage includes bounded list with and without pagination, send/create, get existing, cancel working, follow-up send, missing-task get errors, finite streaming, first-event subscription, and push config create/get/list/delete across gRPC, JSON-RPC, and HTTP+JSON. Streaming rows record event counts plus first-event and completion latency histograms. Known limitation: multi-subscriber subscription, disconnect isolation, terminal-completion subscription, and local HTTP callback fan-out are currently not full wire rows.

## PostgreSQL command attribution

PostgreSQL result rows expose `postgres_phase_latency_ms`,
`postgres_phase_call_count`, and `postgres_phase_calls_per_operation`. The phase
names are stable: `connection_acquire_wait`, `task_get`, `task_upsert`,
`task_history_lock_read`,
`push_config_upsert`, `push_config_get`, `push_config_delete`,
`push_config_list_count`, `push_config_list_select`, `transaction_begin`, and
`transaction_commit`. A command phase counts one invocation per `PQexecParams`
call. Latency timers surround the database command only, rather than result
parsing or service work. When one phase executes multiple commands during one
operation, its latency sample is the cumulative command time for that phase
within the operation; reported percentiles therefore describe per-operation
phase totals, not individual command latency. Phase latency histograms include
successful operations, while call counts include both successful and failed
operations so failures do not hide database work. JSON retains the nested maps,
while CSV and Markdown provide the same totals and calls per measured operation.

Focused scenario fixture preparation must occur before the measured window; setup commands must not be attributed to the named operation.


The current successful PostgreSQL paths intentionally use one `task_get` for task
get; one task-aware `push_config_upsert` for create-config; one
`push_config_get` for a present config; `task_get`, `push_config_list_count`, and
`push_config_list_select` for list; and one `push_config_delete` for delete.
Create-many fan-out eight represents eight independent public API calls, not a
batch SDK call, and therefore executes eight commands rather than sixteen. The
create upsert selects and key-locks the task row in the same statement. A
missing task produces no returned row and maps to `TaskNotFound`; the
task-delete trigger removes configs when deletion wins after creation, so
concurrent deletion cannot leave an orphaned configuration. When the supplied
authoritative task store is not the PostgreSQL task store for the same database
and schema, create retains the lookup-first fallback because the PostgreSQL
statement cannot validate that external store.

Further round-trip reductions require separate SQL-design changes. List could combine existence, count,
and rows, but empty/out-of-range pages and a consistent count need careful
snapshot semantics. Get currently uses a second `push_config_get` only after a
miss to distinguish a missing task from a missing config; a join or tagged query
could preserve that distinction in one command. Whether count-plus-select is
material depends on controlled page-size measurements, so diagnostics alone do
not justify changing it.
