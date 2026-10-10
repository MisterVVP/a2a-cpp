# Performance SUT HTTP I/O

Issue #277 introduces a dedicated HTTP implementation for `performance_sut`.
`RunSutRuntime` selects it with `SutHttpImplementation::kAsynchronous`;
`tck_sut` retains `kBlocking`, its accepted-socket adapter, and its connection
threads. HTTP+JSON, JSON-RPC, agent discovery, SSE, and performance diagnostics
share the existing routes, executor, and stores. gRPC scheduling is unchanged.

## Portable dependency

The test harness uses standalone Asio 1.18 or newer, a header-only Boost Software License
library. Asio provides native epoll (Linux), kqueue (macOS), and IOCP (Windows)
scheduling and owns native socket handles without narrowing Winsock `SOCKET` to
`int`. The implementation uses its stable callback API rather than a custom
poller or a second HTTP protocol parser. The SDK itself has no Asio dependency
when `A2A_ENABLE_TESTING=OFF`.

Install `libasio-dev` on Debian/Ubuntu, `asio` with Homebrew, or the `asio` vcpkg
manifest dependency. `A2A_ASIO_INCLUDE_DIR` supports an existing Asio installation.
The Linux, macOS, and Windows CI test jobs all run `performance_http_io_test`.
A separate `performance_http_io_sut` driver selects the identical HTTP runtime
with gRPC startup disabled for `performance_http_io_tsan_test`; this isolates
ThreadSanitizer from the uninstrumented system gRPC/Abseil libraries. The normal
performance/TCK entrypoints keep gRPC enabled.

## Ownership and scheduling

One `io_context`, run by the main SUT thread, owns the listener, all sockets,
connection state, timers, write offsets, admission, and shutdown. There is one
outstanding read and at most one write per connection. Asio handles interrupted
and would-block operations; completions advance offsets or close/reset the
connection. A half-close drains complete unary requests; stream EOF cancels the
subscription. A malformed or oversized request closes its connection.

A fixed set of single-thread application executors runs incremental parsing,
routing, executor/store work, stream production, and cancellation. The I/O loop
collects bounded raw bytes; reusable buffers hand them to the assigned worker.
Parser state stays on that worker, and completions update byte accounting on
the I/O loop. Accepted connections receive workers
in round-robin order and remain on that worker. This avoids moving protobuf and
allocator working sets between threads. Each connection has at most one
request/stream production job outstanding. A completed response may also have
one cleanup job queued before the next request on that same worker; reuse
does not wait for an extra I/O completion. Closed connections retain their admission slot
until application cleanup finishes, bounding both queued jobs and retained
connection objects during churn.

Response headers and an initial batch of SSE events are prepared in the routing
job. A batch produces at most 16 events and targets 64 KiB; a larger individual
event still obeys the output limit. The next batch is scheduled only after the
previous write drains. Idle streams register a readiness callback that posts to
the I/O loop; they hold no application worker. A timer emits the existing SSE
heartbeat when no event arrives. Callbacks only schedule work, must not throw, and never call `Next` inline.
Publication invokes them under the coroutine promise mutex; callback removal
synchronizes with publishers.

The example SUT's live sessions support `NextFor(0)` and readiness. A live session
without readiness support is rejected by this asynchronous adapter rather than
blocking its worker indefinitely. Finite `Next` calls run on the application
worker. Arbitrary executor/store calls must have their own bounded operation
timeouts; socket cancellation cannot interrupt an arbitrary synchronous call.

## Parser and stream compatibility

`HttpAdapter::AppendInput` and `TryReadRequest` support incremental input with
cached header/body state and retained pipelined bytes. Blocking `ReadRequest`
uses the same parser. `EncodeResponse` supplies the existing validated HTTP
framing to both write paths. Chunked finite and completed live streams retain
connection reuse and response ordering. Only one request is routed at a time
on each connection, including across a stream's final chunk.

`HttpServerResponse::stream_source` supplements `stream_writer`. Both capture
the same session and event encoders; existing blocking callers continue using
`stream_writer`. `HttpStreamSource` provides serialized production, readiness,
producer-queue limits, cancellation, and terminal-event delivery accounting.
The subscription queue budget is opt-in, preserving existing blocking sessions.
If a bounded asynchronous subscriber falls behind, it reports a stream error
and cancels rather than accumulating unbounded events.

Diagnostic measurement lifetimes use request accounting instead of a
thread-owned `shared_mutex` lock. A reset is admitted only after earlier requests
finish; later requests wait in the bounded admission queue until reset completes.
Disconnects release measurement accounting without counting failed writes.
Unary, finite-stream, and connection-reuse counter meanings are unchanged.
Terminal `http_delivery` timing includes asynchronous queuing and socket writes.

## Bounds and backpressure

| Environment variable | Default | Maximum |
| --- | ---: | ---: |
| `A2A_PERF_HTTP_WORKERS` | 4 | 64 |
| `A2A_PERF_HTTP_MAX_CONNECTIONS` | 256 | 4096 |
| `A2A_PERF_HTTP_MAX_INPUT_BYTES` | 1 MiB | 16 MiB |
| `A2A_PERF_HTTP_MAX_OUTPUT_BYTES` | 4 MiB | 16 MiB |
| `A2A_PERF_HTTP_WRITE_SLICE_BYTES` | 64 KiB | 16 MiB |

All values must be positive integers. The write slice also permits deterministic
partial-write tests. HTTP headers are limited to 64 KiB and 128 fields on the
performance path; blocking callers retain their existing header limits.

Reads pause when the input buffer fills and resume after parsing frees space;
TCP then applies backpressure. Output holds one bounded frame/batch per
connection and pauses further stream production until it drains. The live
producer queue is bounded by the input budget. The connection cap pauses accept
admission and resumes it when cleanup releases a slot.

Global queued input and output are therefore bounded respectively by
`max_connections * max_input_bytes` and
`max_connections * max_output_bytes` (256 MiB and 1 GiB with defaults). These are
bounds on queued transport bytes, not total process RSS: decoded requests,
response bodies, temporary encoding buffers, socket buffers, and SDK/store data
also consume memory. The fixed connection/job limits bound those transport
working sets; task-store growth remains governed by the workload.

`A2A_HTTP_IO_RESOURCES` records configured workers, connection limits/peaks,
global byte bounds, and peak queued bytes. It is separate from the existing
performance result schema. The benchmark sampler additionally records actual
process thread count, CPU, RSS, and virtual memory.

## Shutdown

Asio's signal notification or `Stop()` wakes the I/O loop without accept polling.
Shutdown closes the listener and every socket, cancels heartbeat waits, releases
queued request admission, and serializes session cancellation with production.
Connection work guards keep the I/O context alive through cancellation and
application completions. It then drains canceled I/O and joins the fixed workers
before diagnostic snapshots and gRPC shutdown. The TCK shutdown flow is retained.

## Validation and measurements

The socket suite covers fragmented and oversized input, full-buffer pipelining,
partial writes, finite streams and reuse, idle subscriptions, cancellation,
EOF, concurrent connections, slow readers, output limits, accept resumption,
and shutdown with pending reads/writes/streams. Shared-parser and subscription
unit tests cover incremental parsing, encoder parity, readiness detach, and
producer queue overflow.

See [the benchmark evidence](performance-http-io-results.md) for the comparison
parameters, hardware, raw samples, tolerance decisions, and validation outcomes.
