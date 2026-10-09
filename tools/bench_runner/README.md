# Benchmark reports and trends

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
Actions-read token in `GH_TOKEN`. It excludes the current run on reruns and never
uses PR artifacts as baselines. It downloads the raw JSON, so runs predating the
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
`go build ./cmd/...` from this directory. The tests include compiled CLI and
mocked GitHub artifact retrieval coverage and do not require network access.
