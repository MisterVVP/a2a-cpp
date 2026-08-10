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

## Base-to-head performance evidence

The following measurements compare base commit `7e482c5` with this change in
the same container. Component values are medians of five release-mode runs
(50,000 iterations per run); lower is better. The named workloads match the
#85 benchmark bodies. Positive change means less time.

| Component workload | Base (ns) | Head (ns) | Change |
| --- | ---: | ---: | ---: |
| `BM_RestQueryParser_ParseOnly` | 522.3 | 386.3 | +26.0% |
| `BM_RestResponseBuilder_BuildOnly` | 181.0 | 153.2 | +15.3% |
| `BM_JsonRpcEnvelope_ParseOnly` | 23,947.0 | 24,389.3 | -1.8% |
| `BM_JsonRpcEnvelope_SerializeOnly` | 11,281.5 | 11,068.1 | +1.9% |
| `BM_ProtoJson_ResponseToJson_Task` | 26,984.0 | 26,636.1 | +1.3% |
| `BM_JsonRpcTransport_GetTask` | 160,975.0 | 146,028.0 | +9.3% |

Envelope parsing and serialization were not the dominant JSON-RPC opportunity:
they remained within 2%. The full GetTask component was substantially more
expensive because flat request payloads took a `Struct -> JSON -> typed proto`
round trip. JSON-RPC now maps flat scalar request messages directly through
protobuf reflection and retains the ProtoJSON fallback for messages containing
nested, repeated, or otherwise unsupported fields. This removes the round trip
from GetTask, CancelTask, and compatible push-configuration requests without
changing complex SendMessage or streaming parsing.

Wire results used the in-memory store, 500 requests per coordinate, a 0.25-second
warmup, and one run at each concurrency. Values are `base -> head`; errors were
zero in every row. Because the gRPC control varied materially between runs,
small changes should be treated as noise rather than SDK speedups.

### Paginated task list (REST query path)

| Transport | Concurrency | Throughput (ops/s) | p95 (ms) | p99 (ms) | Errors |
| --- | ---: | ---: | ---: | ---: | ---: |
| gRPC | 1 | 1,533.5 -> 1,453.9 | 1.079 -> 0.964 | 6.867 -> 9.119 | 0 -> 0 |
| gRPC | 4 | 3,622.4 -> 2,586.5 | 2.003 -> 3.656 | 2.379 -> 9.501 | 0 -> 0 |
| gRPC | 16 | 3,479.6 -> 3,395.1 | 6.916 -> 6.525 | 7.855 -> 20.195 | 0 -> 0 |
| gRPC | 64 | 2,923.2 -> 2,934.6 | 33.855 -> 29.076 | 37.906 -> 32.693 | 0 -> 0 |
| HTTP+JSON | 1 | 197.0 -> 192.9 | 8.256 -> 9.603 | 14.787 -> 18.885 | 0 -> 0 |
| HTTP+JSON | 4 | 429.1 -> 436.0 | 30.191 -> 27.797 | 36.416 -> 34.281 | 0 -> 0 |
| HTTP+JSON | 16 | 448.7 -> 439.7 | 73.915 -> 71.739 | 88.545 -> 90.333 | 0 -> 0 |
| HTTP+JSON | 64 | 418.3 -> 474.5 | 238.354 -> 194.375 | 301.948 -> 241.295 | 0 -> 0 |

### Existing-task lookup (flat JSON-RPC payload path)

| Transport | Concurrency | Throughput (ops/s) | p95 (ms) | p99 (ms) | Errors |
| --- | ---: | ---: | ---: | ---: | ---: |
| gRPC | 1 | 2,623.9 -> 3,801.6 | 0.469 -> 0.462 | 0.921 -> 0.795 | 0 -> 0 |
| gRPC | 4 | 3,295.3 -> 3,279.4 | 2.599 -> 3.054 | 7.494 -> 14.554 | 0 -> 0 |
| gRPC | 16 | 4,055.2 -> 4,080.9 | 7.275 -> 6.188 | 10.517 -> 14.424 | 0 -> 0 |
| gRPC | 64 | 3,719.5 -> 3,765.8 | 26.211 -> 22.683 | 30.643 -> 25.444 | 0 -> 0 |
| JSON-RPC | 1 | 341.5 -> 336.3 | 5.414 -> 5.049 | 14.631 -> 11.117 | 0 -> 0 |
| JSON-RPC | 4 | 820.9 -> 1,025.9 | 14.806 -> 5.871 | 31.138 -> 26.563 | 0 -> 0 |
| JSON-RPC | 16 | 1,029.7 -> 918.4 | 39.066 -> 42.804 | 48.514 -> 56.359 | 0 -> 0 |
| JSON-RPC | 64 | 929.7 -> 992.3 | 90.806 -> 84.021 | 106.522 -> 91.039 | 0 -> 0 |

The wire data shows modest low-concurrency improvements but does not establish a
broad high-concurrency transport speedup: HTTP and JSON-RPC still plateau well
before gRPC, and run-to-run control variance is significant. The production
changes are therefore justified by the isolated component improvements and
allocation removal; the wire tables are reported as the required end-to-end
check, not as a claim that the concurrency-scaling problem is solved.
