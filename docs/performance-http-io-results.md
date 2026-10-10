# HTTP I/O benchmark evidence

## Comparison contract

The tolerances were recorded before evaluating the implementation: no more than
10% median throughput loss, and p95/p99 increases no larger than the greater of
20% or 0.1 ms, with zero errors. Ten percent allows short measurement and shared
host/client/store noise; the absolute allowance avoids large percentages for tiny
sub-millisecond changes. These limits were never relaxed after measurement.
All final comparisons, including every outlier, are published below.

Baseline: `fe72da4109cd8182a0cccd4c3c84beaae52e8945` (blocking performance SUT).
The branch incorporates main `9958907acf4b0613abde731b8f0d2ef74260c804`; the
intervening change affects Go benchmark trend reporting, not this C++ baseline.
Both SUT binaries use GCC 13.3 Release (`-O3 -DNDEBUG`), PostgreSQL enabled,
subscription diagnostics disabled, and the unchanged baseline wire client.
Asynchronous defaults are one I/O thread, four application workers, and 256
admitted connections. Stores and protocol endpoints match. Binary hashes are in
[metadata.json](performance-http-io-data/metadata.json). The follow-up binary also
includes the final stale-heartbeat guard; none of these finite/unary scenarios
enters the live heartbeat path.
These measurements precede the follow-up Windows graceful-close repair; the
published hashes identify the measured binaries. That repair was validated with
the required performance smoke and socket regressions rather than a new capacity
comparison.

Host: AMD EPYC 7763 virtual machine, Linux x86-64, three visible CPUs, a parent
cgroup two-CPU quota and 8 GiB memory limit (the nested Docker cgroup reports
unlimited). Ubuntu 24.04 userspace uses Asio 1.28.1, protobuf 3.21.12, gRPC 1.51.1,
and PostgreSQL 16.15. This shared host gives harness evidence, not a production
capacity estimate. Build/lint/sanitizer work was paused during measurement.

Three alternating before/after repetitions use 0.5 s warmup and 2 s measurement
at concurrency 1 and 64. In-memory and local PostgreSQL stores use fresh schemas
and a PostgreSQL pool of 64. Churn opens a new connection per operation; idle
runs hold 128 acknowledged reusable connections while measuring SendMessage.
A 20 ms `/proc` sampler records server CPU (100% means one CPU), RSS, virtual
memory, and actual threads. Wire scenarios share a sampling interval including
warmup; resource values are run-level, not per-scenario CPU costs.

## Short-run results

Values are medians across repetitions; latency cells show before → after in ms.
[Raw samples](performance-http-io-data/samples.json) preserve SDK fields, errors,
resource summaries, and shutdown counters. The [comparison](performance-http-io-data/comparison.json)
contains the exact tolerance decision for every row. All runs report zero errors.

| Store | Clients | Workload | ops/s before → after | Change | p50 ms | p95 ms | p99 ms | Tolerance |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | --- |
| inmemory | 1 | Connection churn | 872 → 2559 | +193.4% | 1.133 → 0.368 | 1.265 → 0.497 | 1.428 → 0.580 | Pass |
| inmemory | 1 | SendMessage + 128 idle | 2035 → 1918 | -5.8% | 0.471 → 0.495 | 0.593 → 0.612 | 0.718 → 0.739 | Pass |
| inmemory | 1 | GetTask | 2742 → 2414 | -12.0% | 0.353 → 0.399 | 0.430 → 0.476 | 0.521 → 0.551 | Outside |
| inmemory | 1 | SendMessage | 2118 → 2034 | -4.0% | 0.455 → 0.478 | 0.577 → 0.586 | 0.694 → 0.696 | Pass |
| inmemory | 1 | Finite SSE | 2727 → 2435 | -10.7% | 0.350 → 0.386 | 0.461 → 0.545 | 0.571 → 0.676 | Outside |
| inmemory | 64 | Connection churn | 2697 → 2999 | +11.2% | 19.431 → 18.008 | 50.420 → 44.656 | 67.122 → 58.341 | Pass |
| inmemory | 64 | SendMessage + 128 idle | 4686 → 4441 | -5.2% | 9.379 → 10.064 | 41.728 → 39.865 | 52.579 → 42.939 | Pass |
| inmemory | 64 | GetTask | 6124 → 7047 | +15.1% | 7.101 → 7.108 | 36.654 → 25.999 | 43.186 → 29.469 | Pass |
| inmemory | 64 | SendMessage | 5105 → 4527 | -11.3% | 9.487 → 9.969 | 38.991 → 39.481 | 46.700 → 43.701 | Outside |
| inmemory | 64 | Finite SSE | 5469 → 7112 | +30.0% | 8.323 → 6.959 | 36.558 → 23.363 | 44.585 → 32.968 | Pass |
| postgres | 1 | Connection churn | 424 → 732 | +72.7% | 2.239 → 1.335 | 2.616 → 1.631 | 3.056 → 2.026 | Pass |
| postgres | 1 | SendMessage + 128 idle | 608 → 617 | +1.4% | 1.571 → 1.557 | 1.943 → 2.010 | 2.160 → 2.829 | Outside |
| postgres | 1 | GetTask | 1649 → 1682 | +2.0% | 0.594 → 0.579 | 0.730 → 0.705 | 0.828 → 0.781 | Pass |
| postgres | 1 | SendMessage | 666 → 641 | -3.7% | 1.463 → 1.516 | 1.797 → 1.814 | 2.059 → 3.382 | Outside |
| postgres | 1 | Finite SSE | 706 → 666 | -5.6% | 1.374 → 1.442 | 1.696 → 1.726 | 2.152 → 1.902 | Pass |
| postgres | 64 | Connection churn | 1399 → 1840 | +31.6% | 36.376 → 32.213 | 93.679 → 69.837 | 121.289 → 77.084 | Pass |
| postgres | 64 | SendMessage + 128 idle | 1600 → 1612 | +0.8% | 30.550 → 36.756 | 77.528 → 48.822 | 92.783 → 52.492 | Pass |
| postgres | 64 | GetTask | 3766 → 3438 | -8.7% | 11.435 → 14.099 | 45.160 → 43.329 | 53.374 → 47.008 | Pass |
| postgres | 64 | SendMessage | 1663 → 1649 | -0.9% | 29.239 → 36.912 | 79.177 → 50.023 | 96.609 → 53.940 | Pass |
| postgres | 64 | Finite SSE | 1799 → 1608 | -10.7% | 28.121 → 36.835 | 62.762 → 53.108 | 69.170 → 55.280 | Outside |

14 of 20 short comparisons meet the fixed limits. Every comparison
outside those limits was selected for a longer isolated follow-up, without
changing the decision thresholds. The parameters were recorded before running
[the follow-up plan](performance-http-io-data/followup-plan.json): five alternating
repetitions, 1 s warmup and 10 s steady-state measurement for each selected case.

## Longer follow-up

| Store | Clients | Workload | ops/s before → after | Change | p50 ms | p95 ms | p99 ms | Tolerance |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | --- |
| inmemory | 1 | GetTask | 2505 → 2553 | +1.9% | 0.382 → 0.379 | 0.517 → 0.473 | 0.601 → 0.620 | Pass |
| inmemory | 1 | Finite SSE | 2904 → 2468 | -15.0% | 0.333 → 0.383 | 0.451 → 0.515 | 0.553 → 0.593 | Outside |
| inmemory | 64 | SendMessage | 4241 → 4218 | -0.5% | 9.499 → 10.691 | 43.079 → 38.022 | 51.440 → 43.915 | Pass |
| postgres | 1 | SendMessage + 128 idle | 654 → 635 | -2.9% | 1.508 → 1.545 | 1.795 → 1.854 | 2.022 → 2.093 | Pass |
| postgres | 1 | SendMessage | 653 → 644 | -1.5% | 1.502 → 1.530 | 1.825 → 1.827 | 2.136 → 2.009 | Pass |
| postgres | 64 | Finite SSE | 1768 → 1790 | +1.2% | 28.444 → 32.267 | 66.152 → 47.799 | 81.481 → 51.847 | Pass |

5 of 6 longer comparisons meet the unchanged limits. The remaining
outliers mean the no-material-regression acceptance criterion is not established
on this host. The PR remains draft pending investigation or repeat measurement
on a dedicated host; resource scalability gains do not waive this requirement.
[Raw follow-up samples](performance-http-io-data/followup-samples.json) and the
[comparison](performance-http-io-data/followup-comparison.json) retain variability
and the decision. PostgreSQL can dominate request time; the Python churn client
can become client-bound at high concurrency. These effects and the shared host
limit conclusions about small differences, especially tail latency.

## Resource scalability

| Store | Clients | Workload | Server CPU % before → after | Peak RSS MiB before → after | Peak virtual MiB before → after | Peak threads before → after |
| --- | ---: | --- | ---: | ---: | ---: | ---: |
| inmemory | 1 | churn | 17.0 → 71.5 | 65.7 → 46.2 | 14565.3 → 902.4 | 10 → 13 |
| inmemory | 1 | idle | 40.9 → 40.1 | 38.9 → 33.4 | 2622.6 → 902.6 | 138 → 13 |
| inmemory | 1 | wire | 41.1 → 39.9 | 36.1 → 35.7 | 798.2 → 902.4 | 10 → 13 |
| inmemory | 64 | churn | 71.9 → 86.9 | 157.5 → 50.4 | 45110.6 → 902.6 | 39 → 13 |
| inmemory | 64 | idle | 59.5 → 54.1 | 41.1 → 34.3 | 3167.1 → 902.9 | 201 → 13 |
| inmemory | 64 | wire | 54.7 → 50.2 | 43.1 → 36.5 | 3535.5 → 902.8 | 73 → 13 |
| postgres | 1 | churn | 16.5 → 29.0 | 51.2 → 31.9 | 7504.0 → 908.5 | 10 → 13 |
| postgres | 1 | idle | 22.7 → 22.0 | 37.8 → 32.6 | 2629.1 → 909.1 | 138 → 13 |
| postgres | 1 | wire | 24.4 → 23.6 | 31.6 → 31.8 | 804.3 → 908.5 | 10 → 13 |
| postgres | 64 | churn | 57.2 → 70.5 | 102.3 → 34.2 | 24376.9 → 909.2 | 63 → 13 |
| postgres | 64 | idle | 44.1 → 41.6 | 41.1 → 33.2 | 3133.4 → 909.6 | 201 → 13 |
| postgres | 64 | wire | 42.9 → 40.3 | 38.5 → 32.2 | 3157.3 → 908.9 | 73 → 13 |

Actual threads include gRPC/background runtime threads; HTTP workers remain five.
The bounded implementation removes thread growth with idle connections and churn.
RSS includes task-store growth; faster churn creates more tasks in the same time,
so RSS is not a per-connection allocation estimate. Queued transport byte bounds
and observed peaks appear separately in `A2A_HTTP_IO_RESOURCES` and are asserted
by socket tests.

## Reproduction

Preserve a Release baseline binary and driver before building the branch:

```bash
git worktree add /tmp/a2a-http-before fe72da4109cd8182a0cccd4c3c84beaae52e8945
cmake -S /tmp/a2a-http-before -B /tmp/a2a-http-before/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DA2A_ENABLE_POSTGRES_STORE=ON
cmake --build /tmp/a2a-http-before/build --target performance_sut a2a_wire_performance_driver
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DA2A_ENABLE_POSTGRES_STORE=ON -DA2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS=OFF
cmake --build build-release --target performance_sut a2a_wire_performance_driver
A2A_TEST_POSTGRES_DSN=postgresql://USER:PASSWORD@127.0.0.1:5432/DATABASE \
python3 tools/performance/compare_http_io.py \
  --before /tmp/a2a-http-before/build/tests/performance_sut \
  --after build-release/tests/performance_sut \
  --driver /tmp/a2a-http-before/build/tests/a2a_wire_performance_driver \
  --output http-io-results
```

Keep the client/compiler/store settings identical and pause other CPU work.
The helper uses loopback ports 28120/28121 and creates fresh schemas, so use a
disposable benchmark database. To repeat the longer follow-up, set the same DSN
and run this from the repository root after adjusting the binary paths:

```python
import argparse, importlib.util, json, pathlib, time
spec = importlib.util.spec_from_file_location("comparison", "tools/performance/compare_http_io.py")
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)
plan = json.loads(pathlib.Path("docs/performance-http-io-data/followup-plan.json").read_text())
helper.DURATION = plan["duration_seconds"]
helper.WARMUP = plan["warmup_seconds"]
args = argparse.Namespace(run_id=str(time.time_ns()), driver="/tmp/a2a-http-before/build/tests/a2a_wire_performance_driver")
rows = []
for backend, concurrency, workload, scenario in plan["cases"]:
    helper.SCENARIOS = (scenario,)
    for repetition in range(plan["repetitions"]):
        versions = [("before", "/tmp/a2a-http-before/build/tests/performance_sut"),
                    ("after", "build-release/tests/performance_sut")]
        if repetition % 2:
            versions.reverse()
        for version, binary in versions:
            rows.extend(helper.measure(args, version, binary, backend, concurrency, workload, repetition))
pathlib.Path("followup-samples.json").write_text(json.dumps(rows, indent=2))
pathlib.Path("followup-comparison.json").write_text(json.dumps(helper.aggregate(rows), indent=2))
```

## Validation


The native Debug build has PostgreSQL and subscription diagnostics enabled.
The required gRPC/HTTP+JSON in-memory smoke (two requests, concurrency one, no
warmup/duration) produces 54 rows with zero errors. ASan/UBSan with leak detection
passes 59 focused parser/subscription/socket tests (including both HTTP drivers). ThreadSanitizer passes the
58 focused tests using the HTTP-only driver, with no suppressions. Starting
the combined gRPC SUT under TSan reports an external Abseil graphcycles race; the
isolated driver runs the identical asynchronous HTTP implementation without
starting those uninstrumented system-library threads.

All 671 native tests, exact CI formatting, `./scripts/verify_changes.sh`,
and the separate required `./scripts/run_clang_tidy.sh build` pass (exit 0).
Each socket driver now runs eleven integration cases, including a real heartbeat
followed by cancellation and persistent reuse. Local lint
uses the real clang-tidy 19 executable in parallel, with successful checks cached
only for identical compiler commands, configuration, source, and dependency
headers. Native Linux execution is complete; macOS/Windows execution remains
for the existing PR CI matrix. The maintainer waived the 80.9% baseline TCK gate;
a full TCK result is not claimed. Blocking SUT selection and diagnostic-symbol
isolation remain covered by native regressions.

Cppcheck's warning/portability safety scan passes on the changed I/O, parser,
stream source, and subscription implementation. The added Asio dependency was
reviewed against GitHub's advisory database: CVE-2019-25219 affects versions
before 1.13 and does not affect the required 1.18+ or the tested 1.28.1. The OSV
API was inaccessible through the environment proxy; this is a scoped advisory
review, not a claim of a complete transitive vulnerability audit.
