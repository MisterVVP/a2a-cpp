# Performance benchmarks

The SDK includes C++ microbenchmarks for core hot paths. Benchmarks are run in CI with threshold checks to catch accidental performance regressions.

## Architecture

- **C++ / Google Benchmark** measures SDK internals directly. This avoids Go-to-C++ boundary costs, process startup costs, and HTTP socket overhead when the benchmark is intended to isolate serialization, routing, task store, task lifecycle, UUID generation, or transport handling logic.
- **Go benchmark runner** handles orchestration/reporting. It consumes Google Benchmark JSON, validates fixed thresholds from `thresholds.json`, emits a Markdown summary, and returns deterministic CI exit codes.

The Markdown summary groups results by component so the transport totals and their
parse, conversion, projection, lifecycle, and response-building costs remain easy
to compare as the suite grows.

## Build and run locally

Benchmarks are not built by default. Use a Release build for meaningful numbers:

```bash
cmake -S . -B build-bench \
  -DCMAKE_BUILD_TYPE=Release \
  -DA2A_BUILD_BENCHMARKS=ON

cmake --build build-bench --target a2a_benchmarks -j"$(nproc)"

./build-bench/benchmarks/a2a_benchmarks
```

Run benchmarks with machine-readable output and threshold validation:

```bash
./build-bench/benchmarks/a2a_benchmarks \
  --benchmark_out=benchmark-results.json \
  --benchmark_out_format=json \
  --benchmark_repetitions=5 \
  --benchmark_report_aggregates_only=true

go run ./tools/bench_runner/cmd/a2a-bench-runner \
  --results benchmark-results.json \
  --thresholds benchmarks/thresholds.json \
  --summary benchmark-summary.md
```

You can also build the Go runner once:

```bash
go build -o build-bench/a2a-bench-runner ./tools/bench_runner/cmd/a2a-bench-runner

./build-bench/a2a-bench-runner \
  --results benchmark-results.json \
  --thresholds benchmarks/thresholds.json \
  --summary benchmark-summary.md
```

## Thresholds

`thresholds.json` uses fixed upper bounds in nanoseconds. Initial thresholds are intentionally conservative so the CI job catches major regressions without being flaky on shared runners. To update thresholds intentionally:

1. Run the Release benchmark job several times on comparable CI runners.
2. Use aggregate mean values from Google Benchmark JSON.
3. Set `max_time_ns` to a conservative multiple of observed CI means.
4. Keep the Markdown summary artifact with the PR for review context.

Do not compare Debug or sanitizer benchmark results against Release thresholds. Debug and sanitizer builds add instrumentation and different optimization settings, so their timings are not a valid regression signal.

## Adding a benchmark

1. Add a deterministic C++ benchmark under `benchmarks/` with a stable `BM_<Component>_<Operation>_<Scenario>` name.
2. Avoid real network I/O and avoid logging inside benchmark loops.
3. Move setup outside the measured loop unless setup cost is intentionally being measured.
4. Use `benchmark::DoNotOptimize(...)` and `benchmark::ClobberMemory()` where appropriate.
5. Add a corresponding entry to `thresholds.json`.
6. Run the Go benchmark runner and inspect `benchmark-summary.md`.

## Coverage notes

The current suite covers proto/JSON serialization for every protobuf response shape,
JSON-RPC envelope parsing and serialization, REST query parsing and response building,
task-store projection, serialization-free task lifecycle operations, UUIDv7 task ID
generation, transport mux routing, REST and JSON-RPC transport handling, Agent Card
generation, and the HTTP adapter.
