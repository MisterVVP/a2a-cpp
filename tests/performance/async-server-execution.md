# Async server execution investigation

The starting branch was `perf/grpc-client-streaming` at `5202055` (PR #337). The comparison against `main` uses `56291f5`. That revision has the synchronous transports and lacks the coroutine server targets. The comparison against the starting branch isolates this work.

## Request paths

- **Synchronous HTTP+JSON and JSON-RPC.** The SUT starts a blocking worker per connection. It reads and frames HTTP, prepares and validates the binding request, calls `Dispatcher::Dispatch` on the same worker, serializes the response, then writes it to the same connection. REST GetTask and ListTasks share this path. JSON-RPC adds envelope parsing and response encoding. The connection table uses a mutex and the application store can take its own lock. Requests on separate connections run concurrently. [SUT listener](../sut/tck_sut_http_server.cpp) · [REST binding](../../src/server/transport/rest_server_transport.cpp) · [JSON-RPC binding](../../src/server/transport/json_rpc_server_transport.cpp) · [Dispatcher](../../src/server/dispatcher.cpp)
- **Native HTTP on the starting branch.** The socket loop frames the request, reserves a pending slot, and creates the connection's cancellation state. The listener posts native preparation to its application pool, with the canonical REST GetTask path posted to the SDK runtime instead. The shared mux handles discovery and parsing. A dispatch plan then reserves dispatcher capacity and posts startup to a per-call strand. The dispatcher owns the application lifecycle, interceptors, security checks and coroutine cancellation. The native completion uses a mutex to claim the reply exactly once. At more than one pending HTTP request it posts serialization to the listener application pool. The final write is always posted to the socket's loop. [Event HTTP listener](../../src/server/transport/event_http_listener.cpp) · [Native handler](../../src/server/transport/event_http_native_handler.cpp) · [Async dispatcher](../../src/server/engine/async_dispatcher.cpp) · [Async runtime](../../src/server/engine/async_runtime.cpp)
- **Finite SSE.** The synchronous connection worker drains the stream and writes frames itself. In the native binding, the dispatcher runs the producer and validates the events. Each write posts to the socket loop, suspends the producer and acknowledges the write on its associated executor. A connection's output queue and stream gate protect byte limits, closure, and ordering. The terminal chunk follows the last acknowledged event. These transitions carry backpressure and keep socket operations on their owner. [Native handler](../../src/server/transport/event_http_native_handler.cpp) · [SSE listener](../../src/server/transport/event_http_listener.cpp)
- **gRPC.** The synchronous service prepares metadata and dispatches directly on a gRPC request worker. The callback server adds a call registry protected by a mutex, queues work to the Asio dispatcher, and translates completion into the gRPC reactor's `Finish`. A cancellation or deadline callback queues a signal to Asio, where the request strand serializes coroutine cancellation and dispatcher completion. Neither server uses the HTTP executor. [Synchronous gRPC service](../../src/server/transport/grpc_server_transport.cpp) · [Callback gRPC service](../../src/server/transport/async_grpc_server_transport.cpp)
- **Dispatcher and application.** The dispatcher has separate capacity and active-call registry locks. The per-call strand serializes startup, cancellation-signal installation, application resumption and the terminal hooks. The transport and application each track a distinct terminal event. The HTTP slot lasts through terminal socket write or disconnect, and the dispatcher pending slot lasts through coroutine cleanup. A ready in-memory operation makes a synchronous store call in the coroutine, with no I/O suspension. The PostgreSQL fixture uses a bounded blocking adapter. [Request execution](../../src/server/engine/request_execution.cpp) · [Async dispatcher](../../src/server/engine/async_dispatcher.cpp) · [SUT coroutine executor](../sut/sut_coroutine_executor.h)
- **Client scheduling.** Both closed-loop unary drivers configure one operation per lane. The sync driver keeps one client and worker per lane. The async driver starts one per client and submits the next from its completion. The async gRPC call uses its native callback, then delivers through the SDK completion machinery. Async HTTP registers a curl easy handle with a multi reactor, takes the reactor command-queue lock, and wakes the poller. Curl completion returns the handle to its pool and delegates to the same SDK completion machinery. Comparison needs equivalent lane occupancy as well as client and server CPU measurements. [Wire driver](a2a_wire_performance_driver.cpp) · [Async gRPC client](../../src/client/transport/grpc_async_transport.cpp) · [HTTP async client](../../src/http/http_client_async.cpp) · [Curl reactor](../../src/http/curl_stream_reactor.cpp) · [SDK completion](../../src/core/async_operation.cpp)

## Reproduction

On Linux, use a common Release wire driver and one SUT process per sample. The local runs used GCC 15.3, Boost 1.92, an AMD EPYC 9V74 host with a two-CPU quota, client CPU 0 and server CPU 1. The main and branch builds shared the same gRPC, curl and simdjson installations. Alternate variant order each repetition. Build the baseline and candidate with the same compiler, flags, and dependencies. To reproduce the delayed and isolated cases, build the benchmark-only changes on both server revisions. Keep the same security configuration and connection reuse. The isolated modes expose one fixed HTTP+JSON task and compare the event listener with a pre-encoded response against the native binding with an immediately completing application. The regular in-memory profile runs the real SUT. Set the HTTP pending limit equally for both variants when measuring 256 lanes. The SDK listener's default is 128.

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DA2A_ENABLE_ASIO=ON -DA2A_ENABLE_POSTGRES_STORE=ON -DA2A_BUILD_BENCHMARKS=ON
cmake --build build-release --target performance_sut a2a_wire_performance_driver a2a_benchmarks

python3 tests/performance/compare_server_execution.py \
  --driver build-release/tests/a2a_wire_performance_driver \
  --server baseline async /path/to/baseline/performance_sut \
  --server candidate async build-release/tests/performance_sut \
  --server main sync /path/to/main/performance_sut \
  --transports http_json jsonrpc grpc --concurrency 1 16 64 256 \
  --http-pending-limit 512 --server-cpus 1 --client-cpus 0 \
  --scenarios GetTask_ExistingTask --clients sync --requests 24000 \
  --warmup-seconds 0.2 --duration-seconds 0.5 --repetitions 3 \
  --output perf-artifacts/async-execution/wire.jsonl

build-release/benchmarks/a2a_benchmarks --benchmark_filter='BM_Dispatcher_(Sync|Async)Immediate' \
  --benchmark_min_time=0.3s --benchmark_repetitions=5
```

Repeat the wire comparison for `ListTasks_NoPagination` on HTTP+JSON and `SendStreamingMessage_FiniteStream` on HTTP+JSON and JSON-RPC. Use lanes 1, 64 and 256 for finite streams. Use `--server raw listener ...` and `--server ready immediate ...` for the isolated HTTP GetTask cases. Use `--delay-ms 1 10 50 --scenarios GetTask_ExistingTask` with the instrumented sync and async SUTs for the delay comparison. The sync fixture sleeps on its request worker, and the coroutine fixture waits with an Asio steady timer on the request strand. The timer goes through the dispatcher's usual cancellation slot. The wire runner reports client and server CPU seconds, sampled thread peaks and RSS high-water, along with operation errors and driver timeouts. For successful GetTask and ListTasks samples, it computes the time-weighted outstanding count from the sum of operation latencies divided by the measurement window.

## Experiments

- **A · Native HTTP ingress.** Beyond REST GetTask, routing and protocol preparation can use the same SDK runtime as dispatch. Discovery, JSON-RPC and REST use the common mux. The listener keeps its own pool for generic callbacks and externally driven dispatchers. The test variant changed placement using the existing lifetime drain.
- **B · Shared network and application executor.** A single scheduler would need connection-affine strands for reads, writes, timers and shutdown. Existing public synchronous handler, interceptor, card provider, security and store hooks can block. Keeping those hooks off the network workers still requires a bounded execution boundary. This experiment stopped at design review after tracing those hooks and ownership requirements.
- **C · Coroutine startup.** The prototype let native unary ingress enter the dispatcher strand without a forced queue turn when already on the runtime. It installed the disconnect bridge before starting the coroutine. Public dispatch still posted startup and the per-call strand still owned cancellation-signal installation and terminal hooks. Calling `co_spawn` directly from arbitrary submitters would allow inline application execution and can race cancellation-slot installation against shutdown.
- **D · Unary reply completion.** The prototype encoded and completed replies on the dispatcher strand at every concurrency. It retained the reply claim and connection-affine socket write. At C64 the HTTP+JSON GetTask median dropped from 13,775 to 12,876 req/s and p99 rose from 10.42 to 18.71 ms. Removing the serialization pool handoff increased tail latency under load here. This prototype was discarded.
- **E · Client comparison.** At C1, swapping clients changed absolute rates without eliminating the gRPC GetTask or JSON-RPC GetTask server gap. CPU and lane occupancy matter more at higher concurrency. The measurements support focusing on server scheduling.

## Results

Each wire table reports medians of three interleaved samples unless otherwise specified. The throughput range shows the lowest and highest repetitions. CPU is the server process's share of one CPU over the driver process window, which includes warmup and setup. RSS is the sampled process high-water mark. Errors are summed across repetitions. Errors include any failed request. Timeouts in the separate column mean a wire driver exceeded its process deadline.

With a sync client, the initial starting-branch reproduction showed a C1 async server deficit of 20% for gRPC GetTask, 13% for HTTP+JSON GetTask, 12% for JSON-RPC GetTask and 21% for HTTP+JSON ListTasks. The async client still showed respective deficits of 22%, 8%, 15% and 5%. Successful in-process synchronous dispatch measured 0.43 µs versus 5.18 µs for asynchronous dispatch with one worker, over five repetitions. That benchmark includes the submitting thread's promise/future round trip. The standard deviations were 0.01 µs and 0.13 µs respectively.

### Isolating HTTP listener, dispatch and application work

The first two variants share the fixed task and response headers. The real SUT includes its interceptor, store and populated task response, so its difference also includes extra application and serialization work.

Cells show listener-only → immediate coroutine → real in-memory SUT.

| Binding | Workload | Lanes | Req/s (min–max) | p50/p95/p99 ms | Server CPU % | Threads | Server RSS MiB | Errors/timeouts |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| HTTP+JSON | GetTask | 1 | 12,085 (11,837–13,044) → 9,096 (9,069–9,353) → 4,668 (4,612–4,756) | 0.07/0.12/0.17 → 0.10/0.17/0.21 → 0.20/0.27/0.36 | 22 → 40 → 50 | 6 → 8 → 21 | 11.8 → 12.2 → 14.8 | 0/0 → 0/0 → 0/0 |
| HTTP+JSON | GetTask | 16 | 20,144 (19,453–21,043) → 18,625 (17,378–19,997) → 12,764 (12,642–12,896) | 0.75/1.30/1.60 → 0.82/1.38/1.76 → 1.22/1.83/2.20 | 26 → 68 → 95 | 6 → 8 → 21 | 12.0 → 12.5 → 15.3 | 0/0 → 0/0 → 0/0 |
| HTTP+JSON | GetTask | 64 | 23,174 (22,430–23,984) → 24,369 (23,853–24,744) → 15,639 (15,381–15,805) | 2.61/4.76/5.72 → 2.45/4.31/5.79 → 3.91/5.93/6.86 | 27 → 68 → 91 | 6 → 8 → 21 | 12.6 → 13.4 → 16.5 | 0/0 → 0/0 → 0/0 |
| HTTP+JSON | GetTask | 256 | 21,714 (20,800–21,974) → 24,098 (22,592–24,267) → 14,058 (12,991–14,392) | 11.36/17.33/20.63 → 9.72/16.38/20.55 → 16.77/26.19/34.46 | 25 → 60 → 78 | 6 → 8 → 21 | 14.5 → 16.3 → 20.0 | 0/0 → 0/0 → 0/0 |

### Independent ingress and startup comparison

Native ingress removed route-specific executor selection and showed gains for some workloads. At C64 JSON-RPC GetTask gained 12% over the branch baseline in this round. Adding inline startup raised its p99 from 13.34 ms to 20.14 ms against ingress alone. For C64 JSON-RPC ListTasks, inline startup also cut throughput and raised p99. The selected implementation uses A. The public dispatch path and response serialization placement use the existing scheduling.

Cells show starting branch → A → A and C.

| Binding | Workload | Lanes | Req/s (min–max) | p50/p95/p99 ms | Server CPU % | Threads | Server RSS MiB | Errors/timeouts |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| HTTP+JSON | GetTask | 1 | 4,701 (4,328–4,821) → 4,759 (4,627–4,761) → 4,811 (4,800–4,879) | 0.20/0.25/0.34 → 0.20/0.26/0.30 → 0.20/0.24/0.28 | 51 → 51 → 53 | 21 → 21 → 21 | 14.9 → 14.7 → 14.7 | 0/0 → 0/0 → 0/0 |
| HTTP+JSON | GetTask | 64 | 13,912 (13,195–14,849) → 14,210 (13,976–14,520) → 14,008 (13,477–14,443) | 4.16/7.39/9.68 → 4.20/6.65/9.49 → 4.26/7.04/9.94 | 90 → 90 → 90 | 21 → 21 → 21 | 16.5 → 16.3 → 16.5 | 0/0 → 0/0 → 0/0 |
| HTTP+JSON | ListTasks | 1 | 2,279 (1,951–2,295) → 2,405 (2,248–2,405) → 2,328 (2,214–2,352) | 0.41/0.52/1.08 → 0.41/0.49/0.55 → 0.40/0.51/0.58 | 44 → 43 → 45 | 21 → 21 → 21 | 15.2 → 14.9 → 15.0 | 0/0 → 0/0 → 0/0 |
| HTTP+JSON | ListTasks | 64 | 5,415 (4,059–5,440) → 5,135 (5,112–5,451) → 5,396 (5,329–5,402) | 11.56/17.12/18.97 → 12.07/17.96/21.00 → 11.52/17.18/20.70 | 74 → 71 → 71 | 21 → 21 → 21 | 18.1 → 18.5 → 18.4 | 0/0 → 0/0 → 0/0 |
| JSON-RPC | GetTask | 1 | 2,972 (2,936–3,358) → 3,173 (2,765–3,226) → 3,357 (2,878–3,448) | 0.29/0.38/0.56 → 0.29/0.42/0.48 → 0.28/0.39/0.44 | 56 → 56 → 57 | 21 → 21 → 21 | 15.0 → 14.8 → 14.9 | 0/0 → 0/0 → 0/0 |
| JSON-RPC | GetTask | 64 | 7,988 (7,874–7,990) → 8,968 (8,214–9,327) → 8,480 (7,851–8,778) | 7.63/11.80/14.55 → 6.86/9.74/13.34 → 6.85/13.40/20.14 | 92 → 92 → 92 | 21 → 21 → 21 | 16.9 → 16.9 → 16.8 | 0/0 → 0/0 → 0/0 |
| JSON-RPC | ListTasks | 1 | 2,049 (2,003–2,174) → 2,155 (1,994–2,236) → 2,163 (2,023–2,238) | 0.49/0.57/0.63 → 0.45/0.55/0.69 → 0.44/0.55/0.63 | 43 → 43 → 45 | 21 → 21 → 21 | 15.3 → 15.1 → 15.1 | 0/0 → 0/0 → 0/0 |
| JSON-RPC | ListTasks | 64 | 4,782 (4,491–4,862) → 4,983 (4,916–5,009) → 4,608 (4,367–4,792) | 12.80/20.65/27.11 → 12.63/18.38/21.08 → 12.74/20.06/32.44 | 71 → 70 → 70 | 21 → 21 → 21 | 18.4 → 18.3 → 18.2 | 0/0 → 0/0 → 0/0 |

### Cancellable waits in the real HTTP+JSON SUT

Cells show sync → async on the starting branch. At 256 lanes the async SUT kept 21 threads while sync used 265. The 1 ms and 10 ms timers improved async throughput by 43% and 23% at that concurrency. A 50 ms wait made the two servers nearly equal in throughput and raised async p99 from 64.81 to 72.95 ms. At 16–64 lanes there was little throughput advantage and some runs were slower for async.

| Wait ms | Lanes | Req/s (min–max) | p50/p95/p99 ms | Server CPU % | Threads | Server RSS MiB | Errors/timeouts |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 1 | 741 (724–747) → 729 (711–739) | 1.33/1.43/1.58 → 1.33/1.47/1.84 | 10 → 13 | 10 → 21 | 14.3 → 14.9 | 0/0 → 0/0 |
| 1 | 16 | 9,038 (8,841–9,211) → 7,884 (7,444–7,932) | 1.67/2.52/4.06 → 1.97/2.67/3.23 | 76 → 74 | 25 → 21 | 15.1 → 15.5 | 0/0 → 0/0 |
| 1 | 64 | 12,212 (11,439–12,313) → 12,112 (12,083–13,533) | 5.03/7.29/9.21 → 4.95/7.80/9.86 | 85 → 75 | 73 → 21 | 17.3 → 16.7 | 0/0 → 0/0 |
| 1 | 256 | 9,899 (9,528–10,250) → 14,111 (13,675–14,263) | 22.34/40.85/120.40 → 16.08/26.53/30.07 | 36 → 58 | 265 → 21 | 24.0 → 20.5 | 0/0 → 0/0 |
| 10 | 1 | 96 (95–96) → 96 (96–96) | 10.41/10.55/10.66 → 10.45/10.57/10.81 | 3 → 3 | 10 → 21 | 14.3 → 14.9 | 0/0 → 0/0 |
| 10 | 16 | 1,487 (1,449–1,513) → 1,486 (1,452–1,499) | 10.49/11.14/13.19 → 10.50/11.44/12.65 | 15 → 16 | 25 → 21 | 15.2 → 15.4 | 0/0 → 0/0 |
| 10 | 64 | 5,741 (5,717–5,886) → 5,277 (5,174–5,353) | 10.57/12.75/15.10 → 11.51/14.54/20.25 | 30 → 25 | 73 → 21 | 17.0 → 16.7 | 0/0 → 0/0 |
| 10 | 256 | 10,100 (9,723–10,266) → 12,375 (9,118–12,670) | 22.87/35.13/45.90 → 19.14/25.97/30.68 | 20 → 16 | 265 → 21 | 24.2 → 21.4 | 0/0 → 0/0 |
| 50 | 1 | 20 (20–20) → 20 (20–20) | 50.63/50.88/50.88 → 50.62/50.75/50.75 | 1 → 1 | 10 → 21 | 14.3 → 14.7 | 0/0 → 0/0 |
| 50 | 16 | 313 (310–313) → 311 (308–312) | 50.86/52.34/52.97 → 51.16/52.41/53.45 | 3 → 2 | 25 → 21 | 15.2 → 15.3 | 0/0 → 0/0 |
| 50 | 64 | 1,214 (1,213–1,217) → 1,196 (1,186–1,209) | 51.25/55.17/56.79 → 52.48/55.92/56.99 | 4 → 3 | 73 → 21 | 17.0 → 16.5 | 0/0 → 0/0 |
| 50 | 256 | 4,348 (3,908–4,556) → 4,406 (4,404–4,415) | 51.76/61.03/64.81 → 52.30/63.64/72.95 | 5 → 3 | 265 → 21 | 24.0 → 21.3 | 0/0 → 0/0 |

### Client comparison on a single async server

Cells show sync client → async client. Client occupancy was effectively equal at C1–C64. C256 carries a small occupancy difference caused by starting and draining 256 lanes. The async client was slower for C1 gRPC and faster for C64–C256 gRPC and HTTP+JSON. The C64 and C256 sync HTTP+JSON client consumed over 90% of its assigned CPU. These are client effects on observed throughput and explain why server comparisons use a single driver and mode.

| Binding | Lanes | Req/s (min–max) | Mean in flight | Client CPU % | Client threads |
| --- | --- | --- | --- | --- | --- |
| gRPC | 1 | 8,830 (7,884–9,000) → 7,903 (7,065–8,024) | 1.0 → 1.0 | 44 → 48 | 10 → 16 |
| gRPC | 16 | 27,197 (26,077–27,418) → 28,937 (27,494–30,068) | 15.9 → 16.0 | 88 → 84 | 25 → 16 |
| gRPC | 64 | 29,804 (29,252–31,449) → 36,512 (35,056–36,639) | 63.5 → 63.8 | 88 → 88 | 73 → 16 |
| gRPC | 256 | 22,400 (22,122–23,715) → 29,763 (28,065–33,955) | 247.1 → 252.5 | 83 → 93 | 265 → 16 |
| HTTP+JSON | 1 | 4,553 (4,185–4,670) → 4,402 (4,212–4,602) | 1.0 → 1.0 | 52 → 54 | 3 → 7 |
| HTTP+JSON | 16 | 12,244 (12,172–12,583) → 12,848 (11,414–13,448) | 15.9 → 16.0 | 98 → 99 | 21 → 10 |
| HTTP+JSON | 64 | 14,656 (14,410–15,071) → 16,020 (15,749–16,999) | 63.5 → 63.9 | 96 → 96 | 69 → 10 |
| HTTP+JSON | 256 | 14,137 (13,911–14,210) → 17,722 (16,859–17,729) | 244.8 → 254.2 | 92 → 92 | 261 → 10 |
| JSON-RPC | 1 | 2,884 (2,861–3,295) → 3,133 (3,015–3,211) | 1.0 → 1.0 | 47 → 49 | 3 → 7 |
| JSON-RPC | 16 | 7,167 (7,078–7,367) → 6,981 (6,665–7,514) | 16.0 → 16.0 | 76 → 76 | 21 → 10 |
| JSON-RPC | 64 | 7,806 (7,528–8,292) → 7,883 (7,759–8,027) | 63.4 → 63.6 | 77 → 72 | 69 → 10 |
| JSON-RPC | 256 | 8,262 (8,076–8,584) → 8,767 (8,145–9,069) | 242.5 → 249.4 | 75 → 69 | 261 → 10 |

### Final wire comparison

This table compares all three servers with the same sync driver. Observer clients are used for finite streams. Both async HTTP fixtures used a pending limit of 512 for this matrix. The gRPC coordinates were rerun using the SUT's HTTP base port, from which the driver derives the gRPC endpoint.

Cells show main sync → starting branch async → candidate async.

| Binding | Workload | Lanes | Req/s (min–max) | p50/p95/p99 ms | Server CPU % | Threads | Server RSS MiB | Errors/timeouts |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| gRPC | GetTask | 1 | 10,116 (8,445–10,524) → 8,631 (8,441–8,743) → 6,782 (6,040–8,425) | 0.09/0.16/0.20 → 0.11/0.15/0.21 → 0.11/0.22/0.54 | 55 → 79 → 72 | 11 → 22 → 22 | 13.7 → 14.9 → 14.8 | 0/0 → 0/0 → 0/0 |
| gRPC | GetTask | 16 | 24,877 (20,239–25,897) → 27,345 (23,868–28,065) → 25,821 (24,400–28,184) | 0.56/1.16/2.01 → 0.54/0.88/1.15 → 0.56/1.00/1.43 | 77 → 92 → 92 | 21 → 27 → 27 | 17.8 → 16.7 → 16.8 | 0/0 → 0/0 → 0/0 |
| gRPC | GetTask | 64 | 24,236 (24,012–24,478) → 29,226 (27,775–31,534) → 30,374 (28,971–31,148) | 2.34/4.38/5.27 → 2.00/2.93/4.09 → 2.02/2.89/4.10 | 77 → 79 → 82 | 59 → 27 → 27 | 25.7 → 20.8 → 20.9 | 0/0 → 0/0 → 0/0 |
| gRPC | GetTask | 256 | 16,144 (15,448–20,845) → 22,954 (22,843–22,970) → 20,157 (17,465–24,059) | 13.18/25.80/27.56 → 10.70/13.81/17.44 → 12.10/15.46/22.68 | 65 → 64 → 57 | 120 → 27 → 27 | 45.6 → 29.1 → 28.6 | 0/0 → 0/0 → 0/0 |
| HTTP+JSON | GetTask | 1 | 5,631 (5,207–5,668) → 4,374 (4,327–4,700) → 4,682 (4,601–4,823) | 0.16/0.26/0.32 → 0.20/0.32/0.62 → 0.20/0.30/0.33 | 42 → 50 → 51 | 10 → 21 → 21 | 13.3 → 14.9 → 14.8 | 0/0 → 0/0 → 0/0 |
| HTTP+JSON | GetTask | 16 | 11,626 (11,306–11,812) → 12,291 (12,243–12,600) → 12,147 (11,503–12,670) | 1.31/2.03/2.53 → 1.22/1.97/2.57 → 1.22/2.02/3.17 | 77 → 95 → 95 | 25 → 21 → 21 | 14.3 → 15.4 → 15.4 | 0/0 → 0/0 → 0/0 |
| HTTP+JSON | GetTask | 64 | 13,945 (13,536–14,395) → 15,238 (15,001–15,481) → 13,782 (12,833–14,424) | 4.39/6.73/8.14 → 4.03/6.12/7.07 → 4.11/7.21/15.35 | 91 → 93 → 91 | 73 → 21 → 21 | 16.7 → 16.3 → 16.4 | 0/0 → 0/0 → 0/0 |
| HTTP+JSON | GetTask | 256 | 10,473 (7,141–13,022) → 14,331 (13,171–14,953) → 14,911 (13,605–14,916) | 19.15/42.48/81.60 → 16.34/26.11/29.50 → 15.93/24.86/28.57 | 82 → 80 → 81 | 265 → 21 → 21 | 23.9 → 20.0 → 20.1 | 0/0 → 0/0 → 0/0 |
| HTTP+JSON | ListTasks | 1 | 2,539 (2,047–2,604) → 2,343 (2,070–2,363) → 2,068 (2,041–2,084) | 0.38/0.47/0.60 → 0.41/0.51/0.61 → 0.48/0.54/0.66 | 43 → 43 → 44 | 10 → 21 → 21 | 13.6 → 15.2 → 15.1 | 0/0 → 0/0 → 0/0 |
| HTTP+JSON | ListTasks | 16 | 4,927 (4,477–5,176) → 4,987 (4,951–5,371) → 5,250 (5,171–5,313) | 3.09/4.74/5.67 → 3.08/4.40/5.90 → 3.02/4.23/4.97 | 74 → 84 → 80 | 25 → 21 → 21 | 15.6 → 16.3 → 16.4 | 0/0 → 0/0 → 0/0 |
| HTTP+JSON | ListTasks | 64 | 5,219 (4,997–5,273) → 5,159 (5,052–5,423) → 5,081 (5,040–5,253) | 11.78/18.63/23.77 → 11.54/18.83/25.11 → 11.91/18.29/25.47 | 79 → 77 → 69 | 73 → 21 → 21 | 19.1 → 18.4 → 18.1 | 0/0 → 0/0 → 0/0 |
| HTTP+JSON | ListTasks | 256 | 4,492 (4,320–4,559) → 4,783 (4,496–5,242) → 4,888 (4,879–4,983) | 52.09/84.20/102.46 → 48.04/77.58/95.97 → 48.08/74.98/87.39 | 70 → 64 → 65 | 265 → 21 → 21 | 27.4 → 24.8 → 24.7 | 0/0 → 0/0 → 0/0 |
| HTTP+JSON | Finite stream | 1 | 5,315 (5,096–5,388) → 3,848 (3,589–4,104) → 4,236 (4,030–4,309) | 0.18/0.24/0.32 → 0.23/0.32/0.87 → 0.23/0.29/0.35 | 35 → 51 → 51 | 10 → 21 → 21 | 15.5 → 16.6 → 16.6 | 0/0 → 0/0 → 0/0 |
| HTTP+JSON | Finite stream | 64 | 12,636 (12,302–12,652) → 13,426 (12,415–13,542) → 12,448 (11,777–13,264) | 5.01/7.98/9.95 → 4.50/6.80/9.43 → 4.61/8.40/11.59 | 64 → 77 → 73 | 73 → 21 → 21 | 21.1 → 21.3 → 20.1 | 0/0 → 0/0 → 0/0 |
| HTTP+JSON | Finite stream | 256 | 11,570 (5,012–12,009) → 11,774 (11,753–12,214) → 11,522 (10,895–11,586) | 18.60/35.61/100.55 → 19.16/33.46/69.36 → 19.05/40.08/68.45 | 65 → 66 → 61 | 265 → 21 → 21 | 28.6 → 25.4 → 23.6 | 0/0 → 0/0 → 0/0 |
| JSON-RPC | GetTask | 1 | 3,781 (3,652–3,806) → 2,806 (2,788–2,953) → 2,920 (2,549–3,251) | 0.25/0.34/0.43 → 0.29/0.47/0.92 → 0.31/0.44/0.80 | 50 → 54 → 55 | 10 → 21 → 21 | 13.5 → 15.1 → 14.9 | 0/0 → 0/0 → 0/0 |
| JSON-RPC | GetTask | 16 | 7,460 (6,974–7,766) → 7,301 (7,102–7,471) → 8,063 (8,051–8,094) | 1.98/3.98/5.96 → 2.12/3.24/4.13 → 1.93/2.76/3.58 | 95 → 94 → 94 | 25 → 21 → 21 | 15.0 → 15.6 → 15.5 | 0/0 → 0/0 → 0/0 |
| JSON-RPC | GetTask | 64 | 7,018 (6,496–7,355) → 7,552 (6,943–7,788) → 8,263 (7,318–8,725) | 8.03/17.00/26.97 → 7.93/13.01/17.41 → 7.08/13.19/20.88 | 95 → 93 → 93 | 73 → 21 → 21 | 19.0 → 16.9 → 16.7 | 0/0 → 0/0 → 0/0 |
| JSON-RPC | GetTask | 256 | 6,382 (6,198–6,740) → 8,654 (8,240–8,713) → 8,528 (8,025–9,039) | 33.41/76.18/173.55 → 28.17/39.57/45.33 → 28.22/43.42/48.73 | 48 → 86 → 86 | 265 → 21 → 21 | 32.3 → 22.1 → 20.9 | 0/0 → 0/0 → 0/0 |
| JSON-RPC | Finite stream | 1 | 4,949 (4,403–5,201) → 3,658 (3,492–3,942) → 3,894 (3,744–3,940) | 0.19/0.25/0.32 → 0.25/0.32/0.65 → 0.25/0.30/0.43 | 38 → 51 → 53 | 10 → 21 → 21 | 15.5 → 16.6 → 16.4 | 0/0 → 0/0 → 0/0 |
| JSON-RPC | Finite stream | 64 | 11,985 (10,399–12,168) → 12,289 (11,961–12,706) → 12,199 (11,408–12,475) | 5.37/8.00/9.32 → 4.77/7.29/14.62 → 4.85/7.50/17.33 | 64 → 75 → 70 | 73 → 21 → 21 | 21.1 → 21.1 → 20.2 | 0/0 → 0/0 → 0/0 |
| JSON-RPC | Finite stream | 256 | 10,965 (10,394–11,036) → 9,732 (8,616–10,027) → 10,362 (7,765–11,129) | 22.01/38.93/59.70 → 22.50/49.61/96.52 → 21.34/47.01/74.70 | 68 → 65 → 64 | 265 → 21 → 21 | 29.4 → 24.8 → 23.8 | 0/0 → 0/0 → 0/0 |

### Longer HTTP confirmation

The first comparison showed worse C64 HTTP+JSON GetTask tail latency and lower C1 ListTasks throughput. Five longer repetitions used the same binaries and CPU placement with 300 ms warmup and 1 s measurement. GetTask, which already used dispatcher ingress at the starting point, showed overlapping ranges and similar medians. C1 ListTasks moved from 2,066 to 2,246 req/s in the longer round, with overlapping ranges. C64 ListTasks was level in throughput with p99 about 1 ms higher.

Cells show starting branch → candidate.

| Binding | Workload | Lanes | Req/s (min–max) | p50/p95/p99 ms | Server CPU % | Threads | Server RSS MiB | Errors/timeouts |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| HTTP+JSON | GetTask | 1 | 4,644 (4,546–4,777) → 4,725 (4,559–4,814) | 0.20/0.26/0.34 → 0.20/0.26/0.32 | 52 → 51 | 21 → 21 | 14.9 → 14.8 | 0/0 → 0/0 |
| HTTP+JSON | GetTask | 64 | 14,661 (14,338–15,093) → 14,828 (14,137–15,185) | 4.16/6.50/8.28 → 4.10/6.37/7.70 | 94 → 95 | 21 → 21 | 16.5 → 16.5 | 0/0 → 0/0 |
| HTTP+JSON | ListTasks | 1 | 2,066 (1,999–2,351) → 2,246 (2,023–2,375) | 0.47/0.54/0.83 → 0.42/0.52/0.66 | 44 → 45 | 21 → 21 | 15.2 → 15.1 | 0/0 → 0/0 |
| HTTP+JSON | ListTasks | 64 | 5,403 (5,224–5,549) → 5,404 (5,202–5,446) | 11.64/16.86/19.80 → 11.54/17.21/20.79 | 75 → 72 | 21 → 21 | 18.4 → 18.4 | 0/0 → 0/0 |

### Decision

Use the runtime's ingress executor for native routes when the dispatcher is backed by `AsyncRuntime`. This removes the REST-only classifier and keeps validation, dispatch, and connection lifecycle on their established paths. The clearest unary gain is on JSON-RPC GetTask at C16 and C64, about 10% versus the branch baseline. C64 p99 still increased from 17.41 to 20.88 ms in the final pass. The HTTP+JSON ListTasks improvement in the longer confirmation falls within a wide run-to-run range. Finite stream tails varied, including a C64 JSON-RPC increase from 14.62 to 17.33 ms and a C256 decrease from 96.52 to 74.70 ms. The low-concurrency async deficit remains against synchronous `main`.

The gRPC measurements are a control for the HTTP scheduling change. The final C1 gRPC candidate measured lower than the branch baseline. Both builds use the same gRPC execution path outside the HTTP listener. The C1 result and the HTTP GetTask control motivate treating small differences as noise on this host. The measured C256 native HTTP process stayed at 21 threads versus 265 for the sync server. Retaining the network and serialization boundaries protects the bounded write and cancellation paths while keeping those concurrency gains.

## Local validation

- Release event HTTP regression suite passed all 34 tests, including native discovery, JSON-RPC ingress drain, request limits, finite SSE, backpressure and cancellation.
- The canonical Debug build succeeded. CTest reported one failure across 1,237 registered tests. `AgentCardSigningJsonTest.IndependentRfc8785Corpus` failed on the same two corpus cases on `main` with the same dependency installations.
- The upstream TCK at `5e6f37c2` ran mandatory tests across gRPC, JSON-RPC and HTTP+JSON on the starting branch and candidate. The four sync/async and in-memory/PostgreSQL profiles each reported 216 passed, 13 skipped, 34 deselected and zero failed assertions in both builds. All profiles scored 84.6%. Every mandatory requirement and transport status matched the baseline, including the 16 MUST requirements marked `NOT TESTED`.
- The vulnerability scanner's vendored dependency request received HTTP 403 from the OSV service.
- The final rerun of the other 1,236 CTest entries completed with 1,233 passed and three skipped. The C++/Python gRPC check, HTTP+JSON and JSON-RPC interoperability checks, and the send, subscribe and cancellation checks against the PostgreSQL-backed async SUT passed.
- The standard `verify_changes.sh` run passed with PostgreSQL enabled and the standard signing setting. It covered 1,208 CTest entries with 1,205 passed and three skipped, the CI formatting check, and the full source and test clang-tidy profiles.
- The final performance smoke run reported zero errors across 54 result rows. The runner tests completed with 54 passed and four skipped.
