# Benchmark reports and trends

## SDK sync/async performance matrix

The existing `performance matrix` CI job runs both server modes and publishes
their results in the same `perf-artifacts-normal` artifact and job summary.
Use the [performance runner command](driver.md#compare-synchronous-and-async-servers)
locally. `results.json` contains raw repetitions and `sync_async_comparisons`;
`results.csv` adds client/server execution modes, executor model, resource
counters, and `async_vs_sync_throughput_ratio`. Raw streaming latency fields
remain available in both formats.

Adjacent sync/async columns show throughput and per-run p95/p99
latency, with one row per workload/transport/store/concurrency. Resource and
streaming latency tables also pair modes in each cell as `sync / async`.
Paired in-memory wire comparisons appear first. PostgreSQL wire results remain
sync-only in separate tables. Raw repetitions are kept in JSON/CSV instead of
repeated in the CI summary.

The client column distinguishes synchronous unary calls from callback-driven
async calls. Supported unary workloads run all four client/server combinations
when `--client-modes sync,async` is selected. Streaming retains its common
observer API. The default and CI run each mode once: these are single-run
observations, and throughput ranges are `n/a`. With explicit local repetitions,
ranges show the minimum and maximum; overlapping ranges warrant caution about
small median differences.

CI also runs a focused sustained matrix in `normal/sustained`, using at least 250 ms
warmup on the measured clients, three-second measurements, concurrency 1/64,
and one pass per mode. The same job summary and artifact include these results.
The broad normal matrix retains short coverage runs. Read the sustained results
for performance conclusions; CPU/RSS/thread counters still include setup and
warmup as described below.

The throughput ratio is **async divided by sync**: above 1 is a gain,
below 1 a regression, near 1 parity. With explicit local repetitions, latency
summaries use medians of run percentiles rather than pooled percentiles.
Unmatched cases show `n/a`. Errors,
incomplete operation counts, missing repetitions, and duplicate repetitions
invalidate comparisons and fail the runner; they cannot become speedups.
Ratios are report-only while CI variance is assessed. Existing Google Benchmark
thresholds and PostgreSQL diagnostic profiles retain their behavior.

Resource tables show server and client CPU seconds, sampled peak thread counts,
and process high-water RSS in KiB, where available. Counters cover the entire
driver invocation, including fixture setup and warmup; throughput/latencies
retain the driver's measured-only scope. Linux sampling runs every 100 ms and
can miss short-lived threads. A SUT starts fresh per scenario; the baseline RSS
and resources from its fixture setup are included. Unsupported counters are
`n/a`, not zero. Compare repeated substantial workloads and CPU-placement
metadata before drawing conclusions from small differences.

## Google Benchmark trends

The CI benchmark job keeps the existing hard threshold check and records
`benchmark-trend.json` and `benchmark-trend-summary.md` in its `benchmark-results`
artifact, alongside the raw results and threshold summary. Artifacts are retained
for 90 days, giving each commit a compact history point without publishing Pages
or writing to a branch. Download these JSON files for longer-term analysis before
the artifacts expire.

From the repository root, compare local Google Benchmark outputs:

```bash
go run ./tools/bench_runner/cmd/a2a-bench-trend \
  --results benchmark-results.json \
  --baseline-results previous-main/benchmark-results.json \
  --output benchmark-trend.json \
  --summary benchmark-trend-summary.md \
  --commit "$(git rev-parse HEAD)" \
  --environment 'ubuntu-24.04; Release -O3; threads=4; repetitions=5'
```

The compact schema contains `schema_version` (currently 1), commit SHA, CI run
URL, UTC recording time, measured field, environment description, and sorted
benchmark rows with `name`, `mean_ns`, and `median_ns`. Missing aggregates are
omitted; zero timings are retained. Raw repetitions are averaged and sorted to
compute a median if explicit Google Benchmark aggregates are absent. Values
retain fractional nanoseconds. Standard deviation and coefficient of variation
rows are excluded.

`bash tools/bench_runner/scripts/compare-main.sh` accepts the same command flags
and finds the previous successful **push** run of `ci.yml` on `main`, created
before the current CI run. It requires `GITHUB_REPOSITORY`, the `gh` CLI, and an
Actions-read token in `GH_TOKEN`. The creation-time filter is applied by GitHub
before the fetch limit, so newer runs cannot hide an older rerun's baseline.
It excludes the current run on reruns and never uses PR artifacts as baselines.
It downloads the raw JSON, so runs predating the
trend schema also work. If no baseline exists, the artifact has expired, or the
API is unavailable (including limited fork permissions), it records the current
snapshot and reports the absent baseline. Invalid downloaded JSON fails clearly.
If the current run's creation time cannot be retrieved, baseline lookup is skipped
so a rerun cannot accidentally compare against a newer run.

PR benchmark job summaries show signed mean and median percentage changes for
all benchmarks, including the key benchmarks covered by thresholds. Positive
means slower; negative means faster. Added and missing benchmarks are identified;
missing aggregates and zero denominators show `n/a`. Comparisons use `real_time`
by default; `--time-field cpu_time` selects CPU timings in both input files.
For local comparisons, use results from the same machine and build configuration.
Hosted CI hardware can vary even with an identical runner label.

Signals use the median when both runs provide it, falling back to mean otherwise.
`--noise-percent` defaults to 5: changes within ±5% are marked as noise. The CI
benchmark job uses five repetitions. Trend changes are advisory and never fail a
run; confirm a pattern over multiple runs before using it for roadmap decisions.
The threshold runner and its exit codes are unchanged.

Validate both commands with `gofmt`, `go vet ./...`, `go test -race ./...`, and
`go build ./cmd/...` from `tools/bench_runner`. The tests include compiled CLI and
mocked GitHub artifact retrieval coverage and do not require network access.
