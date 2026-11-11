# SDK v0.6.0 validation

The integrated SDK was compared with v0.5.2 using these revisions:
Baseline: `b58ebd8458bc87bd4ce20641954a17f1c07cdf11`.
Measured SDK source: `fdb1d38c2fe5a506d59b71dee3ee339154f595d1`.

Both builds used GCC 15.3, C++20 and Release optimization on the same Linux
AMD EPYC 9V74 host (two-core CPU quota, 8 GiB memory). Common dependencies were
gRPC 1.51.1, Protobuf 3.21.12, curl 8.23.0 and PostgreSQL 16.15. The candidate
also used Boost 1.92.0 and simdjson 4.6.11.

## Throughput

HTTP+JSON, JSON-RPC and gRPC were tested with unary task creation and finite
streaming, both stores, and concurrency 1/16/64. Each coordinate used a fresh
server/schema, three repetitions of 256 operations, zero wire warmup and a
PostgreSQL pool size of 16. Server order rotated between repetitions.

The table gives the geometric mean of six coordinate medians per transport/store
(two workloads × three concurrency levels), in operations per second.

| Store / transport | v0.5.2 sync | v0.6.0 sync | Sync change | v0.6.0 async |
|---|---:|---:|---:|---:|
| In-memory / HTTP+JSON | 5759.1 | 6062.5 | +5.3% | 3935.9 |
| In-memory / JSON-RPC | 4407.8 | 5315.8 | +20.6% | 3655.4 |
| In-memory / gRPC | 6353.7 | 6277.4 | -1.2% | 6131.7 |
| PostgreSQL / HTTP+JSON | 1933.8 | 2113.0 | +9.3% | 1542.8 |
| PostgreSQL / JSON-RPC | 1589.0 | 1857.9 | +16.9% | 1422.8 |
| PostgreSQL / gRPC | 1862.6 | 2012.1 | +8.0% | 1562.8 |

The matrix and public-client checks completed 92,160 operations with zero errors.
Eight short-run synchronous throughput/p95 alerts were investigated with five
alternating repetitions of 4,096 samples: another 327,680 operations, zero errors.
The worst confirmed throughput change was -3.9%; the worst p95 change was +1.5%.
Async throughput was often lower for this immediately ready fixture, reflecting
scheduling and blocking-adapter overhead. Short-run p99 estimates have limited
statistical power.

## Resource and lifecycle checks

At 64/256/1024 idle or incomplete-header connections, the event HTTP server used
21 threads with zero thread growth, peaked at 25,220 KiB RSS, and used 0.5–1% of
one CPU core. All shutdowns completed within 10 seconds with clients connected.
Sampling was every 20 ms over a two-second dwell, so this is a resource smoke
check, not a long-term memory test.

At concurrency 64, controlled pending push deliveries grew from 7 to 74 process
threads synchronously and from 7 to 10 asynchronously. Counts include caller and
recipient threads; synchronous mode supplies one waiting caller per delivery.
Slow-reader, cancellation, buffer-limit and shutdown checks used the existing
event HTTP integration tests.

PostgreSQL I/O remains synchronous. Separate diagnostic samples attributed
pool waiting, task upsert and push-config lookup; the concurrency-64 medians
were 78.7 ms, 3.06 ms and 0.47 ms respectively. These samples ran alongside builds
and are unsuitable for throughput comparison.

## Validation scope

The initial candidate passed selected ASan/UBSan, Clang 19 TSan, HTTP
interoperability, installed/FetchContent consumer and performance-smoke checks.
Linux vcpkg default and Asio packages built and ran external examples.

Windows/macOS execution, the network-blocked PostgreSQL vcpkg install and C++
dependency vulnerability scanning remain release checks. Unmodified upstream
TCK `104a7b4ae0d83f65c075e3793e322953a830eaba` reported 242 passed, 4 failed and
17 skipped. See [conformance](../compatibility/conformance.md) for the known
assertion conflicts. The mandatory TCK gate has not passed.
