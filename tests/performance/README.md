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

## PostgreSQL tail validation

Run the issue #171 validation matrix against a local PostgreSQL instance with:

```bash
A2A_TEST_POSTGRES_DSN=postgresql://a2a:a2a@127.0.0.1:5432/a2a \
  ./scripts/run_performance_tests.sh --profile postgres-tail \
  --report-dir perf-artifacts/postgres-tail
```

The command runs the five focused in-process scenarios five times at concurrency
1, 4, and 8, using 2,000 requests and a one-second warmup. It writes per-run and
median aggregate JSON, CSV, and Markdown reports under
`perf-artifacts/postgres-tail/`. The report also records `EXPLAIN (ANALYZE, BUFFERS)`
output and verifies the task primary key and push-config creation-order index are
used by the relevant lookups.
