# Async server optimization follow-up

The follow-up uses `63f89b8` from PR #338 as its starting point. The [original stage matrix](request-pipeline.md) and [architecture investigation](async-server-execution.md) remain useful for understanding the earlier experiments.

## Corrected executor topology

Setting a dispatcher worker count previously lowered the listener’s HTTP application pool from four workers to one. The corrected harness controls native ingress, dispatcher workers, HTTP application workers, and the listener’s one- or two-worker network configuration independently. The last setting is available through an internal benchmark entry point. The SUT reports the effective configuration, and the runner verifies it before collecting a sample. The isolated SUT also serves gRPC with the same task fixture and binding as the real SUT.

The production-default topology resolved to one HTTP network worker, four HTTP application workers, and two dispatcher workers with shared native ingress on this host. The benchmark used a 512-request HTTP pending limit for every async variant so C256 could run without admissions failing. Runs pinned the server to CPU 1 and the load driver to CPU 0 on distinct cores with a two-CPU cgroup quota. Both builds used GCC with `-O3 -DNDEBUG`. Warmup and timed intervals were 0.25 s and 0.8 s.

The main sweep covered 576 samples with zero errors across C1, C16, C64, and C256. It compared real and isolated sync, production-default async, and shared ingress with one and four dispatcher workers on all three bindings. The HTTP rows also swept separate ingress with one, two, and four dispatcher workers, one and two application workers under separate ingress with two dispatcher workers, and two network workers independently. The gRPC rows did not repeat combinations that change only HTTP scheduling.

| Variant versus production topology | Workload | Concurrency | Paired throughput | Paired p99 |
| --- | --- | ---: | ---: | ---: |
| Shared, four dispatcher workers | Real JSON-RPC | 64 | +7.5% | −15.8% |
| Shared, four dispatcher workers | Isolated JSON-RPC | 64 | +2.4% | −17.4% |
| Shared, four dispatcher workers | Real JSON-RPC | 256 | −4.0% | +15.1% |
| Shared, four dispatcher workers | Real gRPC | 64 | −12.6% | +47.4% |
| Shared, four dispatcher workers | Isolated gRPC | 64 | −6.0% | −4.1% |
| Separate, two dispatcher workers | Real JSON-RPC | 64 | +1.1% | −14.5% |
| Two network workers | Real REST | 64 | −4.9% | +4.6% |

The entries are medians of three per-repetition percentage changes. The real gRPC throughput penalty with four shared workers occurred in all three pairs, and its p99 change ranged from −4% to +149%. Two network workers reduced real REST throughput in all three C64 pairs on this allocation. These are results for one server core. The local quota could support only that allocation with a separate client core.

The shared-ingress application-pool sweep added 108 samples with zero errors at C16, C64, and C256. Two application workers improved real REST throughput by 7.7% at C64 and 5.3% at C256. At C256 the same setting reduced isolated REST throughput by 7.7% and raised p99 by 16.6%. The worker defaults remain the best-supported configuration across these workloads.

The C64 default-topology medians below show why the local hardware matters. The HTTP client used about 1.06 CPU-seconds across the 1.05-second warmup and sample before startup and drain time. The single client core can limit measured HTTP throughput.

| SUT | Binding | req/s | p50 / p95 / p99 (ms) | Server CPU (s) | Peak server RSS (MiB) | Peak server threads |
| --- | --- | ---: | --- | ---: | ---: | ---: |
| Real | REST | 22,277 | 2.76 / 4.95 / 6.75 | 0.83 | 15.9 | 21 |
| Real | JSON-RPC | 18,064 | 3.45 / 5.82 / 7.79 | 0.89 | 16.1 | 21 |
| Real | gRPC | 46,696 | 1.29 / 1.96 / 2.67 | 1.03 | 21.3 | 27 |
| Isolated | REST | 22,723 | 2.72 / 4.87 / 5.86 | 0.84 | 14.9 | 17 |
| Isolated | JSON-RPC | 18,407 | 3.37 / 5.95 / 8.00 | 0.87 | 15.1 | 17 |
| Isolated | gRPC | 46,241 | 1.28 / 2.11 / 3.39 | 1.03 | 19.6 | 23 |

## JSON-RPC serialization candidate

The candidate reused the ListTasks envelope writer. It serialized each typed unary result once with ProtoJSON, encoded the validated ID with the same library, and wrapped the generated result JSON. The prepared response tests compared the old and new envelopes semantically for GetTask, CancelTask, and SendMessage. They covered nested parts, Unicode and escapes, absent fields, version and extension headers, numeric, string, and null IDs, and errors from wrong response variants or failed dispatch. All 41 JSON-RPC transport tests and HTTP interoperability passed. The async in-memory mandatory TCK run matched the baseline at 84.6%, including every mandatory requirement and transport status.

Five alternating runs of the matched serialization probe put JSON-RPC at **14.59 → 7.54 μs**. The before and after ranges were 14.25–16.50 and 7.17–7.87 μs. The REST control was 5.53 → 5.69 μs.

The wire comparison used the same server topology, seeded task, optimization flags, and dedicated one-core allocation as the topology sweep. Every variant started a fresh process for each sample. The async load-driver sweep used five interleaved repetitions and the sync-client confirmation used three. The timed interval was 0.85 seconds. All 328 samples completed without errors.

| Binding | Concurrency | Real sync throughput / p99 | Real async throughput / p99 |
| --- | ---: | ---: | ---: |
| REST | 1 | −0.7% / −1.7% | +0.4% / +1.4% |
| REST | 16 | +0.7% / +0.1% | +2.3% / −1.4% |
| REST | 64 | +2.0% / +0.8% | −0.8% / −6.2% |
| REST | 256 | +4.0% / −2.8% | +2.3% / −5.1% |
| JSON-RPC | 1 | +9.5% / −4.7% | +7.3% / +1.7% |
| JSON-RPC | 16 | −2.2% / +10.1% | −2.9% / −0.3% |
| JSON-RPC | 64 | +5.5% / −9.2% | +1.3% / −1.3% |
| JSON-RPC | 256 | −5.8% / +13.0% | −0.9% / +2.8% |
| gRPC | 1 | +5.1% / −6.8% | +0.5% / −7.3% |
| gRPC | 16 | −4.2% / +5.6% | −1.8% / +8.1% |
| gRPC | 64 | −2.7% / +8.9% | −0.8% / −15.0% |
| gRPC | 256 | −0.7% / +6.2% | −2.6% / +15.2% |

The table shows median paired changes with the async load driver. Isolated async JSON-RPC gained 7.5% at C1, moved by +0.1% at C16, then lost 2.0% at C64 and 3.5% at C256. On the real async server at C1, the median rate rose from 5,529 to 5,960 req/s and server CPU time fell from 0.54 to 0.46 seconds per sample. Peak RSS remained about 14.8 MiB. On the real sync server at C256, the median rate fell from 18,736 to 17,147 req/s and p99 rose from 25.97 to 28.65 ms.

At C256, the real synchronous server’s median paired p99 rose 9.0% with the sync client and 13.0% with the async client. The sync client confirmed throughput losses in all three C256 sync-server pairs. The async client used roughly a full core at C256, which limits how much can be inferred about the server alone. These measurements show a serialization win and a repeatable C1 win. The C256 wire result rules out shipping this candidate.

## Async request scheduling

A separate diagnostic binary timed the real native HTTP GetTask path in the listener, native handler, and dispatcher. It recorded elapsed intervals into preallocated memory and wrote them after shutdown. The two repetitions for each REST and JSON-RPC ingress and concurrency setting included warmup. All 16 wire samples completed without errors. The probe adds timestamp and recording overhead, so use it to compare stages and identify queueing. The intervals cannot be added to predict application latency.

The C64 table reports pooled p50 / p99 in microseconds on one server core. Both ingress modes used one network worker, four HTTP application workers, and two dispatcher workers.

| Observed interval | REST shared | REST separate | JSON-RPC shared | JSON-RPC separate |
| --- | ---: | ---: | ---: | ---: |
| Network loop post to native ingress execution | 12.68 / 183.46 | 10.56 / 91.04 | 27.90 / 296.04 | 25.58 / 198.78 |
| Binding preparation to dispatcher call | 3.16 / 17.33 | 3.31 / 10.66 | 3.60 / 26.53 | 3.55 / 27.92 |
| Admission, request state creation, and registry insertion | 0.56 / 1.61 | 0.63 / 1.74 | 0.59 / 2.08 | 0.60 / 1.83 |
| Strand post to callback | 11.09 / 198.12 | 11.19 / 112.24 | 29.11 / 321.35 | 24.39 / 232.10 |
| Coroutine spawn to first instruction of the dispatch coroutine | 0.68 / 3.85 | 0.78 / 2.96 | 0.54 / 5.49 | 0.46 / 3.34 |
| Application result to transport delivery | 0.15 / 0.40 | 0.15 / 0.44 | 0.14 / 0.50 | 0.13 / 0.39 |
| Coroutine return to cleanup callback | 4.48 / 140.65 | 3.03 / 74.69 | 9.67 / 240.02 | 5.15 / 107.56 |
| Reply ready to posted serialization start | 17.39 / 186.87 | 16.20 / 133.48 | 41.88 / 390.52 | 49.41 / 403.20 |
| Response payload encoding | 8.37 / 37.53 | 8.32 / 46.84 | 15.40 / 62.24 | 14.92 / 76.93 |
| Terminal reply post to the network owner | 21.77 / 200.12 | 21.62 / 162.89 | 57.36 / 390.41 | 59.51 / 437.99 |
| Async write submission to socket callback | 2.79 / 28.21 | 2.84 / 31.60 | 2.49 / 31.17 | 2.45 / 32.45 |

At C1 JSON-RPC serialization was inline, with a scheduling median of 0.06–0.07 μs. The real socket write interval was 14.66–14.94 μs and was timed directly at the write callback. At C64 most JSON-RPC replies posted serialization to the application pool. Queueing in that pool, on the strand, and on the return to the network owner produced much longer tails than coroutine startup or socket writes in this allocation.

The dispatcher’s explicit strand post preserves its guarantee that admission never runs application code inline. Boost.Asio’s coroutine startup can dispatch inline, and the strand keeps application hooks and cancellation serialized. The listener’s reply claim provides exactly-once delivery and its network loop owns writes, connection state, and admission release. Removing these transitions changes those guarantees. The trace does identify a possible smaller experiment for a host with more cores. A bounded queue of terminal responses could coalesce multiple posts to the same network loop while preserving one socket owner, per-connection ordering, backpressure, and shutdown drains. Its sparse-load wakeup and high-load fairness would need separate measurement. The current evidence does not justify changing the runtime scheduling policy.

## Reproduce the topology sweep

Build `performance_sut` and `a2a_wire_performance_driver` in the same optimized Asio-enabled build. Set `server_cpus` and `client_cpus` to disjoint physical cores. On a sufficiently provisioned host, use server lists `1`, `1,2`, and `1,2,3,4` with client `0`, running the sweep separately for each server allocation. The runner rejects overlapping SMT siblings and inadequate visible CPU quotas. Set the explicit alternate network count to 1 on hosts whose automatic configuration uses two network workers.

```bash
sut="$build/tests/performance_sut"
variants=()
for family in real fixture; do
  sync=sync
  async=async
  if [[ "$family" == fixture ]]; then sync=pipeline-sync; async=pipeline-async; fi
  variants+=(--server "$family-sync" "$sync" "$sut" --server "$family-default" "$async" "$sut")
  for n in 1 4; do
    label="$family-shared-$n"
    variants+=(--server "$label" "$async" "$sut" --executor "$label" "$n" shared)
  done
  for n in 1 2 4; do
    label="$family-separate-$n"
    variants+=(--server "$label" "$async" "$sut" --executor "$label" "$n" separate
               --only-transports "$label" http_json jsonrpc)
  done
  for n in 1 2; do
    label="$family-app-$n"
    variants+=(--server "$label" "$async" "$sut" --executor "$label" 2 separate
               --http-workers "$label" auto "$n" --only-transports "$label" http_json jsonrpc)
  done
  label="$family-network-2"
  variants+=(--server "$label" "$async" "$sut" --http-workers "$label" 2 auto
             --only-transports "$label" http_json jsonrpc)
done
python3 tests/performance/compare_server_execution.py \
  --driver "$build/tests/a2a_wire_performance_driver" --output "$output" \
  --require-dedicated-cores --server-cpus "$server_cpus" --client-cpus "$client_cpus" \
  --require-topology --seed-pipeline-task "${variants[@]}" \
  --scenarios GetTask_ExistingTask --clients async --http-pending-limit 512 \
  --concurrency 1 16 64 256 --requests 120000 --warmup-seconds 0.25 --duration-seconds 0.8 --repetitions 3
python3 tests/performance/summarize_server_execution.py "$output" > "$summary"
```

The CSV contains throughput, p50, p95, p99, CPU time, RSS, peak threads, and error counts. It includes throughput and p99 ranges across runs. CPU time and resource peaks cover the sample’s client process lifetime, including warmup.

To repeat the shared-ingress application-pool sweep, keep the real and fixture async default rows, add variants with `--http-workers LABEL auto 1` and `--http-workers LABEL auto 2`, and use `--transports http_json jsonrpc --concurrency 16 64 256`. The dispatcher and ingress defaults apply to all three variants.
