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
