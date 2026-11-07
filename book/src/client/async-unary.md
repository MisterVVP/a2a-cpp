# Async unary calls

`A2AClient` and `ClientTransport` expose opt-in callback methods alongside their
synchronous methods. Each returns `core::Result<core::AsyncHandle>`:

| Async method | Callback result |
| --- | --- |
| `SendMessageAsync` | `Result<SendMessageResponse>` |
| `GetTaskAsync` | `Result<Task>` |
| `ListTasksAsync` | `Result<client::ListTasksResponse>` |
| `CancelTaskAsync` | `Result<Task>` |
| `CreateTaskPushNotificationConfigAsync` | `Result<TaskPushNotificationConfig>` |
| `GetTaskPushNotificationConfigAsync` | `Result<TaskPushNotificationConfig>` |
| `ListTaskPushNotificationConfigsAsync` | `Result<ListTaskPushNotificationConfigsResponse>` |
| `DeleteTaskPushNotificationConfigAsync` | `Result<void>` |

The protobuf types in this table are in `lf::a2a::v1`. Existing synchronous methods
and `StreamObserver` remain available.

```cpp
lf::a2a::v1::GetTaskRequest request;
request.set_id("my-task");
a2a::client::CallOptions options;
options.timeout = std::chrono::seconds(5);

auto operation = client.GetTaskAsync(request,
    [](a2a::core::Result<lf::a2a::v1::Task> result) {
      if (result.ok()) {
        ConsumeTask(result.value());
      } else {
        ReportError(result.error());
      }
    }, options);
if (!operation.ok()) {
  ReportError(operation.error()); // initiation rejected; no callback follows
}
// Continue application work. To request cancellation:
if (operation.ok()) {
  operation.value().Cancel();
}
```

Use `HttpJsonTransport::CreateDefault` or `JsonRpcTransport::CreateDefault` for
libcurl async I/O. A channel-backed `GrpcTransport` uses gRPC's native unary
callback API. HTTP requests share the existing four-shard libcurl multi reactor
pool with synchronous requests and streams, preserving connection caching. There
is no SDK worker per in-flight unary request. The process starts four shared
completion workers and one deadline thread lazily on the first accepted async
call. gRPC also manages its own native runtime threads.

## Completion and ownership

A rejected initiation returns an error and never calls the completion callback.
Rejections include an empty callback, a closed client, unavailable completion
infrastructure, or exhausted admission capacity. Once accepted, a call invokes
its callback exactly once, including validation, network/protocol errors,
cancellation, deadline expiry and shutdown. Success, cancel, deadline and shutdown
compete for one terminal claim; the winner determines the result. Cancellation
after a result has been selected does not replace it.

Callbacks can run before the initiating method returns. Calls may complete in any
order, and callbacks for different operations may run concurrently. Application
completion callbacks and `AfterCall` run on shared completion workers, away from libcurl reactors and native gRPC callback threads. Callback
exceptions are contained. Keep captures alive until callback completion; dropping
an `AsyncHandle` leaves the operation running. The SDK owns the request's network
storage, the callback, and the copied options needed by interceptors. Callback
code can start more calls or destroy its client. Avoid blocking the shared
completion workers waiting for other callbacks.

`AsyncHandle` is copyable. `Cancel()` is thread-safe, idempotent, and requests
network cancellation of this outbound call. Use `CancelTaskAsync` to request a
task state transition. `Cancel()` does not wait for the callback. `IsActive()`
becomes false once a terminal result has been selected. `Wait()` waits for the callback
to return when called outside completion workers; inside an SDK callback it is a
no-op to prevent callback cycles. It is an observation aid, not another result
channel. Use callback delivery to access the result.

`A2AClient::Destroy()`, transport `Shutdown()`, and destruction cancel outstanding
unary calls. Shutdown from outside SDK callbacks drains their callbacks.
Reentrant shutdown from an SDK callback cancels calls and lets already queued
callbacks finish with owned state. Keep observer-owned state alive until any
concurrent `StreamObserver` callbacks return as well. The gRPC transport also
drains native unary cleanup; contexts, messages and stubs stay alive through native completion.
Simultaneous async initiations and `Destroy()` are supported. As with other C++
objects, callers must keep the client object alive while invoking its methods.

## Deadlines, errors and interceptors

`CallOptions` retains headers, authentication hooks/providers, extensions,
protocol version, TLS settings and timeout. Request preparation, authentication
hooks and `BeforeCall` execute on the initiating thread. Unary network work runs
on shared libcurl reactors or gRPC's native async machinery. HTTP mTLS still
requires a custom requester.

The timeout starts at transport admission, after `BeforeCall`, and includes
request preparation and network work. Transport default timeouts apply when no
explicit timeout is supplied (30 seconds by default). Nonpositive A2A timeouts
expire immediately. Raw `http::Client::SendRequestAsync` uses a 30-second default
when its request timeout is zero. Logical cancellation and shutdown return
`ErrorCode::kNetwork` with protocol code `AsyncHandle::kCancelled` (`CANCELLED`).
Deadline expiry uses `AsyncHandle::kDeadlineExceeded` (`DEADLINE_EXCEEDED`),
including native network deadline results. Other protocol and network errors
retain the synchronous decoders and mappings.

An interceptor snapshot belongs to each async call. `BeforeCall` runs in
registration order. `AfterCall` runs once in reverse order, before the application
callback, and observes the final selected result even on cancel/shutdown. A start
rejected after `BeforeCall` pairs it with `AfterCall` on the initiating thread;
there is no application callback for that rejected start. Interceptor exceptions
are contained, matching synchronous behavior. Adding an interceptor affects
subsequent calls. Interceptors, custom request ID generators and authentication
hooks/providers must support concurrent initiations and completions when shared.

## Discovery and custom transports

`DiscoveryClient::FetchAsync` and `FetchExtendedAgentCardAsync` use the same
completion model and accept `CallOptions`. `CreateDefault` supplies a native
libcurl fetcher. Async discovery has a separate thread-safe cache with the same
configured TTL, validates cards and verifies original JSON before parsing. A
verifier snapshot is retained for each call; setting a verifier clears both
caches, and cache hits reverify original JSON. Extended cards are always fetched.
Serialize verifier changes and synchronous discovery calls with initiation, as
with the existing synchronous discovery API. `DiscoveryClient::Shutdown()` closes
async discovery. Copies share async cache and operation ownership.

Existing custom synchronous transports and HTTP requesters continue to compile.
They do not acquire a blocking worker adaptation: the inherited async hooks
complete with an unsupported-operation validation error. Custom transports
implement protected `StartSendMessage`, `StartGetTask`, and the other `Start*`
hooks using `core::AsyncCompletion<T>`. Copy request data needed after return,
register a cancellation signal with `RegisterCancellation`, and call `Complete`
with the final `Result<T>`. The token isolates callbacks and enforces once-only
completion; duplicate or late completions are ignored. Override
`DefaultAsyncTimeout()` when needed. Custom transport shutdown must close its
shared `async_scope_` before releasing network resources.

The HTTP transport constructors optionally accept `HttpAsyncRequester`. Discovery
accepts `HttpAsyncFetcher`. Inject native async I/O that returns an accepted
`AsyncHandle`; synchronous-only injected requesters are never called for async
operations. HTTP and discovery use the reusable
`http::Client::SendRequestAsync(Request, Completion<Response>)` primitive.

The completion runtime admits at most 4096 operations through callback return.
Each default HTTP operation also owns a raw HTTP operation and consumes a second
admission slot. Async HTTP response bodies are limited to 16 MiB and response
headers to 64 KiB; exceeding the limit completes with a network error. Each HTTP
client bounds native async transfers at 4096 until reactor cleanup,
including canceled transfers. Idle easy-handle pools retain at most 64
handles per category. These bounds are process-wide admission and response
limits, not a promise to allocate the maximum capacity eagerly.

## Validation and measurements

The portable `async_client_test`, `async_http_transport_test`,
`async_http_network_test` and `async_grpc_client_test` targets cover the contract,
protocol semantics and native I/O. The installed consumer test builds the
[example](../../../examples/apps/async_unary_client/main.cpp) using exported SDK
targets. Existing Linux, macOS and Windows CI test jobs include these tests;
libcurl-specific tests are conditional on libcurl support.

`async_unary_benchmark` compares synchronous callers with callback-driven bounded
concurrency against the same live SUT. For example, after starting `tck_sut`:

```sh
build/tests/async_unary_benchmark http_json http://127.0.0.1:50061/a2a sync 1000 32
build/tests/async_unary_benchmark http_json http://127.0.0.1:50061/a2a async 1000 32
build/tests/async_unary_benchmark grpc 127.0.0.1:50062 async 1000 32
```

Use `jsonrpc` with the SUT JSON-RPC endpoint for the third binding. Reports include
errors, request throughput, p50/p95 latency and sampled client thread count (Linux;
zero means unavailable elsewhere). Both modes use the same `ListTasks` request,
client, cache warmup, server and build configuration. The benchmark advertises
the deterministic SUT's required `urn:a2a:tck:required-extension` in both modes.
Sync mode uses one application caller per concurrency slot; async mode starts replacements from
callbacks. Measurements include callback dispatch and periodic resource sampling.
Do not compare these numbers to streaming or server coroutine benchmarks.


### Local comparison

A Linux x86-64 run used GCC 14, Release builds, libcurl 8.14, gRPC 1.51,
1024 loopback `ListTasks` calls per case, and concurrency 1, 8 and 32. The SUT
used the in-memory store. The same candidate executable ran both modes;
compilation and linting were paused during measurement. These short local runs
illustrate dispatch costs and thread use, rather than predicting deployment
capacity. All 18 network cases completed without errors.

At concurrency 32:

| Binding | Mode | Requests/s | p50 ms | p95 ms | Peak sampled threads | Peak sampled RSS MiB |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| gRPC | Sync | 14786 | 1.572 | 2.525 | 40 | 17.4 |
| gRPC | Async | 17312 | 1.728 | 2.572 | 15 | 17.6 |
| HTTP+JSON | Sync | 15551 | 1.127 | 3.347 | 37 | 15.0 |
| HTTP+JSON | Async | 12387 | 1.617 | 4.789 | 10 | 14.4 |
| JSON-RPC | Sync | 11712 | 1.832 | 4.307 | 37 | 16.1 |
| JSON-RPC | Async | 10717 | 2.179 | 4.763 | 10 | 15.4 |

At concurrency 8, sync/async throughput was 10007/10776 requests/s for gRPC,
16954/12464 for HTTP+JSON and 11031/9001 for JSON-RPC. At concurrency 1,
async dispatch increased p50 latency from 0.086 to 0.150 ms for gRPC,
0.101 to 0.134 ms for HTTP+JSON and 0.099 to 0.138 ms for JSON-RPC. The shared
completion pool adds fixed overhead at low concurrency. Async is useful for
bounded application thread use; it does not guarantee higher throughput.
Thread counts include application and native runtime threads. RSS was sampled
externally from `/proc` every 5 ms and is approximate.

The unchanged synchronous parsing benchmark was also built at release baseline
`377375c` and at this implementation with identical Release configurations.
Median CPU time over five samples of at least 0.1 seconds per case was:

| Benchmark | Tasks | Baseline microseconds | Candidate microseconds | Change |
| --- | ---: | ---: | ---: | ---: |
| HTTP+JSON ListTasks | 1 | 4.611 | 4.923 | +6.8% |
| HTTP+JSON ListTasks | 20 | 65.992 | 67.346 | +2.1% |
| HTTP+JSON ListTasks | 200 | 650.291 | 638.242 | -1.9% |
| JSON-RPC ListTasks | 1 | 9.665 | 9.403 | -2.7% |
| JSON-RPC ListTasks | 20 | 71.739 | 72.039 | +0.4% |
| JSON-RPC ListTasks | 200 | 672.389 | 651.528 | -3.1% |

All six cases remain below the repository's absolute benchmark thresholds.
The smallest HTTP+JSON case shows a 0.312 microsecond increase; these short
measurements include noise and do not establish a regression bound.
