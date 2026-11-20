# Matched GetTask pipeline measurements

The REST and JSON-RPC probes use one completed task, a fixed task and context ID, a warm in-memory store, A2A 1.0, the required extension, and no history. Within each binding, synchronous and asynchronous runs start from the same requests and return the same task. The wire matrix also runs the real performance SUT with the same seeded task, retaining its normal executor, interceptor, and store paths. Fixed requests to both SUTs confirmed equivalent JSON and headers for sync, shared ingress, and separate ingress, with response sizes of 97 bytes for REST and 131 bytes for JSON-RPC. JSON-RPC object key order varied between processes. The gRPC wire rows check the effect of changing dispatcher workers on another binding served by the same SUT.

## Stage costs

Times are microseconds per call, reported as the median of seven repetition means. The benchmark ran with GCC, `-O3 -DNDEBUG`, and affinity to two distinct cores on an AMD EPYC 9V74 host. The async probes use one worker. Framing, validation, interceptors, store access, and serialization exercise the code both hosts call and show the same measurement in both columns.

| Stage | REST sync | REST async | JSON-RPC sync | JSON-RPC async |
| --- | ---: | ---: | ---: | ---: |
| HTTP request framing | 0.568 | 0.568 | 0.840 | 0.840 |
| HTTP mux routing | 0.148 | 0.157 | 0.180 | 0.184 |
| Protocol parsing, validation, and dispatch preparation | 1.558 | 1.558 | 2.525 | 2.525 |
| Interceptors | 0.042 | 0.042 | 0.042 | 0.042 |
| In-memory task lookup and copy | 0.073 | 0.073 | 0.071 | 0.071 |
| Dispatcher startup with shared ingress | 0.044 | 1.265 | 0.045 | 1.430 |
| Dispatcher startup with separate ingress | 0.044 | 9.051 | 0.045 | 9.320 |
| Dispatcher completion with shared ingress | 0.040 | 0.077 | 0.041 | 0.077 |
| Dispatcher completion with separate ingress | 0.040 | 0.180 | 0.041 | 0.181 |
| Response serialization | 6.029 | 6.029 | 14.842 | 14.842 |
| HTTP response framing | 0.106 | 0.106 | 0.107 | 0.107 |
| Loopback write completion | 1.724 | 9.487 | 1.714 | 9.374 |

Routing measures the two mux entry points with matching route order and no-op handlers to isolate normalization and matching from binding work. Protocol validation uses the actual binding's prepared dispatch. Dispatcher startup ends at entry to the first interceptor, with admission and worker scheduling inside the async interval. The shared probe submits from the dispatcher's worker. The separate probe submits from outside its pool. Dispatcher completion runs from the post-dispatch hook to the synchronous return or async callback. These intervals include clock and probe overhead. Serialization measures the binding's real response continuation with the task already available. Write completion measures an inline blocking write or an executor post followed by an asynchronous write and its completion handler. A peer drains and verifies the same preframed bytes after each timed write. The stage results come from separate probes and cannot be summed to predict wire latency.

The main measured sync–async differences are startup and socket scheduling. Submitting from the dispatcher pool reduced async startup to 1.3–1.4 μs in this probe. JSON-RPC serialization also costs about 2.5 times REST serialization for the same task. Routing and the hot task lookup are each below 0.25 μs.

## One-core wire comparison

The server ran on CPU 1 and the client on CPU 0 with a two-CPU cgroup quota. Seven interleaved repetitions covered one and 64 concurrent requests, for 490 samples and zero errors. Async topologies used one, two, or four dispatcher workers on the same one-core server allocation. The two-worker shared configuration is the PR's current topology and is the reference for the paired changes below.

| Async topology | Real REST | Isolated REST | Real JSON-RPC | Isolated JSON-RPC | Real gRPC |
| --- | ---: | ---: | ---: | ---: | ---: |
| Shared, 1 worker | +3.1% / −20.1% | −5.7% / +7.1% | −2.9% / +2.4% | −3.0% / +4.0% | −7.4% / +7.9% |
| Shared, 4 workers | +1.3% / +0.1% | −0.1% / +6.4% | +0.9% / −6.2% | −0.1% / +7.8% | −2.8% / −1.1% |
| Separate, 1 worker | −3.1% / +13.8% | −3.7% / +1.2% | −4.5% / +3.5% | −5.5% / +14.2% | −2.5% / +8.4% |
| Separate, 2 workers | +2.1% / −12.4% | −5.0% / +5.4% | −3.1% / +5.7% | −4.4% / +6.6% | −3.3% / −0.5% |
| Separate, 4 workers | +1.3% / −15.8% | −4.9% / +0.7% | −5.1% / +1.7% | −5.7% / +4.2% | −5.7% / +5.3% |

Each cell shows the median paired throughput change and p99 change at concurrency 64. The reference medians for real REST and JSON-RPC were 22.7k and 19.0k requests/s at 6.06 and 6.13 ms p99. The isolated medians were 23.7k and 19.0k requests/s at 5.02 and 5.85 ms p99. Separate ingress with two workers reduced isolated REST throughput in all seven pairs and isolated JSON-RPC throughput in six of seven, despite helping real REST. Shared ingress with four workers had small gains on the real HTTP workloads alongside higher isolated p99. The tails varied across repetitions. For that four-worker configuration, the isolated JSON-RPC p99 change ranged from −43% to +60%.

At concurrency one, the synchronous server had higher throughput in every pair for both HTTP bindings and both SUTs. The median paired gain over the async reference was 25–37%. At concurrency 64, the synchronous server had lower median paired throughput in all four HTTP workload groups.

Five repetitions with the async load driver at concurrency 64 added 150 samples with zero errors. Four shared workers gained 1.8% in real REST, 2.2% in isolated REST, 6.2% in real JSON-RPC, and 2.9% in isolated JSON-RPC. Real gRPC throughput fell 11.5% and its p99 rose 11.5%, with throughput lower in all five pairs. The HTTP client used close to its full core in both client runs, so small throughput differences are specific to this end-to-end setup. No configuration met the throughput and tail-latency criteria across the real and isolated workloads. The production worker count and ingress configuration are unchanged.

The two- and four-core server allocations require at least three and five cores including the client. The local quota supported only the one-core run.

## Reproduce

Configure a Release build with tests, Asio, and `A2A_BUILD_BENCHMARKS=ON`. Build `a2a_benchmarks`, `performance_sut`, and `a2a_wire_performance_driver`. Run the stage probe with two available CPU IDs from distinct cores.

```sh
taskset -c 0,1 "$build/benchmarks/a2a_benchmarks" \
  --benchmark_filter='^RequestPipeline/' --benchmark_min_warmup_time=0.04 \
  --benchmark_min_time=0.08s --benchmark_repetitions=7 --benchmark_out=stages.json
```

Set the server and client CPU lists to disjoint physical cores visible to the process. The wire runner checks Linux affinity, SMT siblings, and the visible cgroup v2 CPU quota. Use the same client core for each allocation. For example, with five available cores and sufficient quota, use server lists `1`, `1,2`, and `1,2,3,4`, with client `0`. Worker counts and core counts are independent. Shared ingress uses the dispatcher pool for native HTTP routing and protocol preparation. Separate ingress uses a single application worker for that same path and keeps the dispatcher pool size fixed. Both use the same socket executor.

```bash
sut="$build/tests/performance_sut"
variants=(--server real-sync sync "$sut" --server fixture-sync pipeline-sync "$sut")
for ingress in shared separate; do
  for workers in 1 2 4; do
    variants+=(--server "real-$ingress-$workers" async "$sut"
               --executor "real-$ingress-$workers" "$workers" "$ingress")
    variants+=(--server "fixture-$ingress-$workers" pipeline-async "$sut"
               --executor "fixture-$ingress-$workers" "$workers" "$ingress")
  done
done
python3 tests/performance/compare_server_execution.py \
  --driver "$build/tests/a2a_wire_performance_driver" --output "$output" \
  --require-dedicated-cores --server-cpus "$server_cpus" --client-cpus "$client_cpus" \
  --seed-pipeline-task "${variants[@]}" --transports http_json jsonrpc grpc \
  --scenarios GetTask_ExistingTask --clients sync --concurrency 1 64 \
  --requests 50000 --warmup-seconds 0.22 --duration-seconds 0.65 --repetitions 7
```

For the async-client confirmation, omit the two synchronous server entries and use `--clients async --concurrency 64 --repetitions 5`.
