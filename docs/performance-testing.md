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

### Subscription terminal phase diagnostics

Subscription attribution is compiled out of normal SDK builds. Enable runtime
collection with the environment switch; the runner automatically configures a
separate optimized diagnostics build with the private CMake option enabled:

```bash
A2A_SUBSCRIPTION_DIAGNOSTICS=1 \
  ./scripts/run_performance_tests.sh
```

The normal build remains in `build/performance`; diagnostics use the isolated
`build/performance-subscription-diagnostics` tree. `A2A_PERF_BUILD_DIR` changes
the normal build-tree base, and the runner appends the diagnostics suffix for
profiling runs so a diagnostics-enabled binary cannot be reused as a normal
benchmark binary.

Wire results keep `server_subscription_diagnostics` and
`client_subscription_diagnostics` separate because the SUT and SDK client run
in different processes. Each phase contains `count`, `total_ns`, and `max_ns`.
`server_cancel_task_total` and `terminal_publication_total` are total timers and
must not be added to their nested phase measurements.

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

- `PushConfig_Get` uses one immutable config fixture, `PushConfig_List` uses a fixed three-config fixture, and
  `PushConfig_Delete` uses one independently pre-seeded task/config pair per operation. Warmup deletes use separate
  task/config pairs, so they cannot consume measured fixtures. `PushConfig_Create` retains one measured create against
  a pre-existing task.
- `PushNotify_EndToEndManyConfigs` creates a task, writes eight push-notification configs, sends one task update, lets the push service list configs from the configured store, builds the update payload, and invokes the local recording callback once per config.
- `PushConfig_ListManyConfigs` uses a task and eight configs seeded before warmup and measures only the list operation for that fixed fan-out.
- `PushDelivery_CallbackFanout` uses preloaded configs and a prebuilt payload seeded before warmup, then measures only the in-process callback delivery loop. It does not create tasks, create configs, query the store, or perform network I/O inside the timed operation.
- `PushConfig_CreateMany` uses a task seeded before warmup and measures only eight real `CreateTaskPushNotificationConfig` calls. On the same-storage PostgreSQL path, each call performs one atomic task-aware `push_config_upsert` command with no separate `task_get`.
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
`PushConfig_List`, `PushConfig_Delete`, `SendStreamingMessage_FiniteStream`,
`SubscribeToTask_FirstEventLatency`, and `IdleStream_ClientCancellationLatency`.
The historical `SendStreamingMessage_FiniteStream` and
`SubscribeToTask_FirstEventLatency` rows preserve the original one-client-per-worker
topology so before/after comparisons remain equivalent to older reports. HTTP+JSON
and JSON-RPC additionally run `SendStreamingMessage_FiniteStream_SharedClient` and
`SubscribeToTask_FirstEventLatency_SharedClient`, where every measured worker uses
one shared `A2AClient`. These shared-client rows are separate scalability
coordinates and must not be substituted for the historical rows when evaluating
regressions or issue acceptance. gRPC does not run the shared-client variants.
The idle-stream cancellation scenario seeds a task, establishes a real subscription,
waits for its initial event, and then measures only the synchronous local
`StreamHandle::Cancel()` call. It does not invoke the protocol-level `CancelTask`
operation or ask the server to publish a terminal event. For HTTP transports it
retains the shared-client stress topology because it has no historical baseline.
The wire driver otherwise reuses one client/transport per worker thread so
measured operations do not recreate gRPC channels or HTTP transport objects. The
libcurl-backed HTTP path submits both unary and streaming requests through a
process-wide pool of at most four CURLM reactors. Reusable easy-handle slots keep
their reactor assignment for connection-cache locality. Each client also aligns
its first unary and streaming handles to one reactor shard, allowing common
request-to-stream sequences to reuse that shard's connection cache while
additional concurrent handles remain distributed across the bounded pool. List scenarios run before mutating lifecycle scenarios and
seed a fixed fixture of 20 tasks, then measure only `ListTasks` calls, keeping
the listed task set bounded in CI. Multi-subscriber subscription, disconnect isolation, terminal-completion subscription, and callback fan-out remain SDK in-process rows in this implementation; they are not duplicated as transport rows and must not be interpreted as full `wire_tck_sut` coverage.

Focused wire fixtures are also prepared before timing: existing-task lookup uses a shared task; push create uses a
pre-existing task; push get uses a shared immutable config; push list uses a fixed three-config set; and every push
delete operation owns a distinct single-config task. Warmup uses a separate fixture namespace. Consequently, lower latency in
these rows reflects corrected benchmark attribution rather than an SDK runtime optimization.

For PostgreSQL, each focused operation executes one matching store command: `task_get` for `GetTask_ExistingTask`,
`push_config_upsert` for `PushConfig_Create`, `push_config_get` for `PushConfig_Get`, `push_config_list_select` for
`PushConfig_List`, or `push_config_delete` for `PushConfig_Delete`.

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

### Attributed PostgreSQL write profile

Use `--profile postgres-write` to reproduce the four write-saturation signals
without wire-transport work. The profile fixes the pool size at 64, runs
`SendMessage_CreateTask`, `SendMessage_FollowUpExistingTask`,
`PushConfig_Create`, and `PushConfig_CreateMany` independently at concurrency
1, 4, 16, and 64, and records five repetitions. A distinct schema is used for
every scenario, coordinate, and repetition, preventing accumulated rows or
another scenario's locks from changing the comparison.

While each scenario runs, the runner samples `pg_stat_activity` and records
session state, concurrent active sessions, idle transactions, and PostgreSQL
wait-event type/event. It also takes before/after snapshots of
`pg_stat_database`, `pg_stat_wal`, and the `MultiXactMember` and
`MultiXactOffset` rows from `pg_stat_slru`. The resulting
`postgres_database_diagnostics` object in `results.json` contains sampled wait
counts and deltas for transactions, tuple writes, cache activity, WAL bytes,
WAL writes/syncs, full WAL buffers, PostgreSQL-reported block/WAL timing, and
MultiXact SLRU block activity. The local task-aware push path now coordinates
create/delete with transaction-scoped advisory locks, so its MultiXact deltas
should remain zero; non-zero activity points to other or legacy row-lock work.
These server observations complement the per-operation
`connection_acquire_wait`, `task_upsert`, `task_history_snapshot`, and
`push_config_upsert` phase latencies already emitted by the SDK driver.
`task_history_snapshot` is the non-locking snapshot read used by optimistic
history appends. `task_history_lock_read` remains a distinct legacy/locking
phase so reports do not misattribute optimistic snapshot latency or retries to
row-lock acquisition.

```bash
A2A_TEST_POSTGRES_DSN=postgresql://a2a:a2a@127.0.0.1:5432/a2a \
./scripts/run_performance_tests.sh --profile postgres-write \
  --report-dir perf-artifacts/postgres-write
```

The profile also captures `EXPLAIN (ANALYZE, BUFFERS, WAL)` output for
representative task-conflict and push-config insert writes inside a rolled-back
transaction. Interpret the evidence before changing production code:

- increasing `WAL:WALWrite` or `IO:WALSync` samples, WAL sync time, and WAL
  bytes with negligible acquisition wait indicates server durability
  throughput, not pool starvation;
- `Lock:transactionid` or tuple-lock samples concentrated on an update fixture
  indicate same-row conflict contention;
- rising buffer reads or read time and an unexpected scan in the captured plan
  indicate a plan, index, or cache issue;
- high `idle_in_transaction` is a transaction-scope defect and should remain
  zero for these autocommit writes.

The diagnostics use cumulative cluster counters, so run this profile against an
otherwise idle PostgreSQL instance. Wait events are sampled observations rather
than exact durations. Each scenario is isolated specifically so its counter
delta and wait samples can be correlated with one performance row.

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
`task_history_snapshot`, `task_history_lock_read`,
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

The combined PostgreSQL push-list statement is attributed to
`push_config_list_select`; `push_config_list_count` remains part of the stable
diagnostic schema but the final list path does not issue a separate count
command. The local task-aware create statement, including the task-lock helper,
is attributed entirely to `push_config_upsert`; it does not emit a separate
`task_get`, `transaction_begin`, or `transaction_commit` phase.

Focused scenario fixture preparation must occur before the measured window; setup commands must not be attributed to the named operation.


## PostgreSQL push-configuration query paths

PostgreSQL stores distinguish **storage coordinates** (effective libpq `host`,
`hostaddr`, port, database, `target_session_attrs`, and schema) from
**pool connection identity** (those coordinates plus the effective PostgreSQL
role).
Passwords and raw connection strings are excluded. Exact storage-coordinate
equality confirms local authority, a different database or schema confirms
external authority, and other endpoint differences are uncertain. Separately
constructed stores can supply a stable, non-secret `storage_authority_id` to
resolve that ambiguity. The ID is an operator assertion and must reflect the
actual storage authority: equal non-empty IDs prove local authority and different
non-empty IDs prove external authority; a one-sided ID remains uncertain.
Database/schema differences remain external regardless of the ID. URI and
keyword DSNs therefore share the local path when libpq reports the same effective
logical options or when matching explicit authority IDs prove locality.

The effective role does not participate in storage authority. Same-storage
stores with different roles still use the local atomic create path. Pool
connection identity is used only to keep connections within one pool
consistent; it does not establish policy equivalence between independent pools.
PostgreSQL RLS expressions can depend on per-session settings, including custom
GUCs supplied through libpq `options`, so the one-command list shortcut requires
the task and push stores to share the exact pool and local storage authority.
Separate pools always perform the authoritative task lookup first, even when
endpoint and role metadata match.
Persistent out-of-band session changes such as `SET ROLE` or custom policy GUCs
on pooled connections are unsupported.

### Create/update

For local authority, `CreateOrUpdateForTask` validates the request and then
executes one `push_config_upsert` statement on one push-store lease. The
statement calls the schema-qualified `SECURITY DEFINER` lock helper, which
checks caller `SELECT`, rejects row-level security and snapshot-isolation modes,
takes a shared transaction-scoped advisory lock derived from the
schema-qualified task table and complete task ID, validates task existence, and
feeds the result into `INSERT ... ON CONFLICT DO UPDATE ... RETURNING 1`. Task
deletion takes the matching exclusive advisory lock in a `BEFORE DELETE`
trigger. The caller does not perform a separate `task_get`, explicit
`BEGIN`/`COMMIT`, post-write revalidation, or compensating cleanup. If the task
is absent, the statement returns no row and maps to `TaskNotFound`. Existing
config IDs retain normal update semantics. The advisory lock and upsert are one
PostgreSQL statement, so concurrent deletion cannot create a locally owned
orphan under the supported read-committed isolation.

Role differences do not create a special split-role transaction path. A
same-storage push role must have task-table `SELECT` and helper `EXECUTE`; the
push-lock helper owner needs schema `USAGE` plus task-table `SELECT`, and the
task-delete lock helper owner needs schema `USAGE`. Task-table `UPDATE` is not
required for advisory locking.

For confirmed external authority, the supplied `TaskStore` remains
authoritative: the SDK performs its task lookup and then executes one external
push upsert with `local_postgres_task=FALSE`. An uncertain PostgreSQL identity
is not treated as external and is rejected before the push write. The
`AFTER DELETE` cleanup trigger removes only rows marked
`local_postgres_task=TRUE`, preserving externally owned rows. The direct
`PushNotificationStore::CreateOrUpdate` API has no authoritative `TaskStore` and
therefore remains on the external-provenance upsert path.

### Get/list

`GetConfig` remains a push-store-only API. PostgreSQL uses one
`push_config_get` statement and one push-store lease for present configs,
missing config IDs, and an absent push-config collection; it does not perform
an authoritative task-store lookup.

For list, task and push stores sharing one connection pool and local storage
authority use one `push_config_list_select` statement and one lease. That
statement combines task
existence, total count, and page selection. With separate pools, `ListForTask`
validates task authority first and then runs one combined push-list statement,
regardless of matching endpoint/role metadata. A shared pool with different
schema/authority coordinates also uses the fallback. The same task-first path is
used for external/custom task stores.

### Command-count contract

For PostgreSQL task and push stores, the final request-path command counts for
valid request shapes are:

| Path | Task-store commands | Push-store commands | Total PostgreSQL commands | Pool acquisitions |
| --- | ---: | ---: | ---: | ---: |
| same-storage create/update, any role | 0 | 1 | 1 | 1 |
| shared-pool list, valid request | 0 | 1 | 1 | 1 |
| separate-pool PostgreSQL list | 1 | 1 | 2 | 2 |
| external-authority create/update | 1 | 1 | 2 | 2 |
| external-authority list | 1 | 1 | 2 | 2 |
| direct get | 0 | 1 | 1 | 1 |

For a non-PostgreSQL external/custom task store, the external create/list rows
still perform one PostgreSQL push command, plus the authoritative task-store
operation outside PostgreSQL. Create/list requests rejected during argument
validation may perform the authoritative task lookup first to preserve
task-first error ordering; those validation paths are not represented by the
successful/valid rows above.

This preserves the command-count acceptance criterion from issue #187 for the
normal same-storage `PostgresStoreFactory` layout: successful create is one
PostgreSQL command, and `PushConfig_CreateMany` at fan-out eight performs eight
`push_config_upsert` commands instead of the previous eight `task_get` plus
eight upserts. The same path preserves `TaskNotFound`, update semantics, and
concurrent-delete safety.

These are structural command counts, not benchmark results. Throughput and
latency claims must come from controlled current-run artifacts for
`PushConfig_Create`, `PushConfig_CreateMany`, and
`PushNotify_EndToEndManyConfigs`; this documentation intentionally does not
carry forward performance numbers from earlier implementations.
