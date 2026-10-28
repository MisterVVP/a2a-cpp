# Performance benchmarks

See the [mdBook guide](https://mistervvp.github.io/a2a-cpp/performance/microbenchmarks.html).

Documentation source: [book/src/performance/microbenchmarks.md](../book/src/performance/microbenchmarks.md).

### Async unary dispatcher overhead

When C++20 cancellation is available, `BM_Dispatcher_SyncImmediate` and
`BM_Dispatcher_AsyncImmediate` use the same in-memory task response, one outstanding
request, and no interceptors or security policy. Run both with
`--benchmark_filter=BM_Dispatcher_ --benchmark_repetitions=5`. The async case uses
one fixed SDK worker and includes admission, scheduling, lifecycle, and completion
handoff to the caller. These are immediate-completion overhead measurements; they
do not measure asynchronous network/database I/O or the benefit of overlapping
pending operations. The fake-transport unit suite separately verifies that many
pending requests progress on a bounded worker pool.

### Native gRPC pending-operation evidence

With tests and C++20 cancellation enabled, build `async_grpc_execution_evidence`
and run it with a request count and backend hold in milliseconds, for example
`./build/tests/async_grpc_execution_evidence 32 10` or `128 10`. It compares sync and
callback services over native gRPC using the same completion-queue client and externally
released backend fixture. JSON includes application/process thread counts and
client p50/p95 latency. `32 0` exercises immediate completions. See the
[guide](../book/src/server/async-grpc.md#reproducing-thread-and-latency-evidence)
for workload limits and measured results. This fixture does not benchmark actual
asynchronous network or database backends.
