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
