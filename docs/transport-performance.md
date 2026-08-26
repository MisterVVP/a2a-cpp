# Transport implementation and performance

REST, JSON-RPC, gRPC, SSE, and HTTP-adapter implementation files live in the
`src/client/transport` and `src/server/transport` directories. Public SDK
headers remain under `include/a2a/client` and `include/a2a/server`; moving an
implementation file must not change those installed include paths.

## REST query parsing

The REST query parser is shared by routing and the component benchmark. Typical
A2A requests contain a small number of unescaped parameters. For that common
case, the parser reserves the final hash-table capacity before insertion and
copies each key and value directly. It only performs character-by-character URL
decoding when a component contains `+` or `%`. Encoded input continues to use
the validating decoder, including the same malformed-encoding errors.

Use `BM_RestQueryParser_ParseOnly` to isolate this work. Validate a transport
change against the wire suite as well, because a component improvement alone
does not demonstrate better request throughput. The primary comparison uses
the in-memory store and HTTP+JSON, JSON-RPC, and gRPC at concurrency 1, 4, 16,
and 64. gRPC is the control for changes in the host or test environment.

On the reference development container, five release-mode runs of the isolated
parser workload used by `BM_RestQueryParser_ParseOnly` had a median of 445.1 ns
before the change and 349.5 ns after it, a 21.5% reduction. Each run parsed two
million three-parameter queries. Results from other hosts should be compared on
the same machine and build configuration rather than against these absolute
times.

Run the component benchmark in a release build:

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DA2A_BUILD_BENCHMARKS=ON
cmake --build build-bench --target a2a_benchmarks
./build-bench/benchmarks/a2a_benchmarks \
  --benchmark_filter='BM_RestQueryParser_ParseOnly'
```

Run the corresponding wire comparison with:

```bash
A2A_PERF_TRANSPORTS=grpc,http_json,jsonrpc \
A2A_PERF_STORE_BACKENDS=inmemory \
A2A_PERF_CONCURRENCY=1,4,16,64 \
./scripts/run_performance_tests.sh
```

## Synchronous streaming parsing profile

PR #223 was measured against its exact parent rather than against an older
published artifact. The baseline was `fc34a18` and the parsing-optimized head
was `65a8da4`. Both revisions were built independently in Release mode and run
on the same host. Each table cell is the median of five request-count runs with
this configuration:

```bash
A2A_PERF_TRANSPORTS=grpc,http_json,jsonrpc \
A2A_PERF_STORE_BACKENDS=inmemory \
A2A_PERF_SCENARIOS=SendStreamingMessage_FiniteStream,SubscribeToTask_FirstEventLatency \
A2A_PERF_REQUESTS=1000 \
A2A_PERF_CONCURRENCY=1,4,16,64 \
A2A_PERF_WARMUP_SECONDS=0 \
A2A_PERF_DURATION_SECONDS=0 \
./scripts/run_performance_tests.sh
```

The following results show baseline/head throughput in operations per second
and baseline/head p95 latency in milliseconds. Positive percentages in the
latency columns are improvements; positive throughput percentages are gains.

### `SendStreamingMessage_FiniteStream`

| Transport | Concurrency | Throughput baseline/head (change) | First-event p95 baseline/head (improvement) | Completion p95 baseline/head (improvement) |
|---|---:|---:|---:|---:|
| gRPC | 1 | 1371.4 / 1167.0 (-14.9%) | 0.894 / 0.950 (-6.2%) | 1.089 / 1.151 (-5.7%) |
| gRPC | 4 | 2460.5 / 2074.9 (-15.7%) | 1.981 / 2.270 (-14.6%) | 3.070 / 3.256 (-6.1%) |
| gRPC | 16 | 2061.4 / 1863.7 (-9.6%) | 10.799 / 17.017 (-57.6%) | 19.226 / 23.622 (-22.9%) |
| gRPC | 64 | 1774.3 / 1692.6 (-4.6%) | 48.624 / 46.661 (+4.0%) | 56.577 / 55.217 (+2.4%) |
| HTTP+JSON | 1 | 604.1 / 584.5 (-3.2%) | 2.355 / 2.297 (+2.4%) | 2.536 / 2.532 (+0.2%) |
| HTTP+JSON | 4 | 1673.1 / 1617.3 (-3.3%) | 3.237 / 2.950 (+8.9%) | 3.410 / 3.194 (+6.3%) |
| HTTP+JSON | 16 | 1640.2 / 1632.0 (-0.5%) | 31.817 / 31.737 (+0.3%) | 32.168 / 31.878 (+0.9%) |
| HTTP+JSON | 64 | 1533.3 / 1588.8 (+3.6%) | 64.929 / 66.561 (-2.5%) | 65.037 / 67.656 (-4.0%) |
| JSON-RPC | 1 | 376.0 / 381.8 (+1.6%) | 3.410 / 3.964 (-16.3%) | 3.648 / 4.485 (-23.0%) |
| JSON-RPC | 4 | 1192.2 / 1197.4 (+0.4%) | 4.738 / 5.038 (-6.3%) | 5.183 / 5.994 (-15.7%) |
| JSON-RPC | 16 | 1207.5 / 1246.3 (+3.2%) | 34.532 / 35.158 (-1.8%) | 35.269 / 35.397 (-0.4%) |
| JSON-RPC | 64 | 1086.3 / 1180.5 (+8.7%) | 80.725 / 81.076 (-0.4%) | 80.934 / 81.392 (-0.6%) |

### `SubscribeToTask_FirstEventLatency`

| Transport | Concurrency | Throughput baseline/head (change) | First-event p95 baseline/head (improvement) | Completion p95 baseline/head (improvement) |
|---|---:|---:|---:|---:|
| gRPC | 1 | 428.7 / 381.9 (-10.9%) | 1.093 / 1.256 (-14.9%) | 2.777 / 3.581 (-29.0%) |
| gRPC | 4 | 694.9 / 637.3 (-8.3%) | 2.690 / 3.357 (-24.8%) | 9.635 / 9.824 (-2.0%) |
| gRPC | 16 | 629.4 / 614.8 (-2.3%) | 24.928 / 23.574 (+5.4%) | 38.718 / 38.773 (-0.1%) |
| gRPC | 64 | 560.0 / 567.8 (+1.4%) | 55.630 / 56.634 (-1.8%) | 108.277 / 105.012 (+3.0%) |
| HTTP+JSON | 1 | 320.8 / 313.0 (-2.4%) | 2.210 / 2.178 (+1.4%) | 3.572 / 3.691 (-3.3%) |
| HTTP+JSON | 4 | 681.2 / 665.0 (-2.4%) | 4.762 / 3.976 (+16.5%) | 10.607 / 10.996 (-3.7%) |
| HTTP+JSON | 16 | 628.9 / 609.3 (-3.1%) | 38.968 / 37.656 (+3.4%) | 49.124 / 47.453 (+3.4%) |
| HTTP+JSON | 64 | 563.8 / 582.3 (+3.3%) | 101.686 / 100.186 (+1.5%) | 146.016 / 143.950 (+1.4%) |
| JSON-RPC | 1 | 194.9 / 180.1 (-7.6%) | 2.899 / 3.237 (-11.7%) | 5.028 / 5.832 (-16.0%) |
| JSON-RPC | 4 | 382.2 / 385.8 (+1.0%) | 9.987 / 12.928 (-29.4%) | 24.443 / 25.454 (-4.1%) |
| JSON-RPC | 16 | 387.2 / 376.1 (-2.9%) | 43.938 / 43.512 (+1.0%) | 59.520 / 59.745 (-0.4%) |
| JSON-RPC | 64 | 341.9 / 338.2 (-1.1%) | 151.230 / 165.990 (-9.8%) | 231.627 / 230.996 (+0.3%) |

All 240 measured wire rows completed without operation errors. At concurrency
64, the unchanged gRPC control changed by -4.6%/+1.4% in throughput and by
+4.0%/-1.8% in first-event p95 for the two scenarios. The HTTP results do not
show the required 15% p95 improvements after accounting for that control, so
the parsing optimization is useful allocation hygiene but does not resolve the
streaming latency target.

The HTTP diagnostics reported 1,064 accepted connections for 1,000 measured
operations at concurrency 64 (and 1,001 at concurrency 1) for both HTTP+JSON
and JSON-RPC. This matches the current adapter behavior: streaming responses
force connection closure, so almost every measured stream pays a new
connection and server-accept lifecycle. The p95 increase from concurrency 1 to
64 is also present in HTTP+JSON, which does not perform JSON-RPC envelope
conversion. Together with the negligible parsing delta, this identifies
connection establishment/teardown and associated server scheduling as the
dominant remaining measurable HTTP-specific lifecycle cost. Per-stream worker
creation still occurs, but these results do not isolate it as dominant.

No shared worker or finite-stream keep-alive change was added: the former is
not justified by the profile, while the latter requires a separately validated
HTTP framing/lifecycle design to preserve cancellation and interoperability.
Accordingly, the concurrency-64 latency acceptance criteria remain open and
issue #209 must not be considered resolved by this change.
