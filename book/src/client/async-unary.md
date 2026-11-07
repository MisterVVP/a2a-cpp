# Async unary calls

Include `a2a/client/async_client.h` and obtain the opt-in callback facade with
`auto async = client.Async()`. `A2AClient` keeps its synchronous methods in
`client.h`; `AsyncClient` defines callback methods in its own header. Each async
method returns `core::Result<core::AsyncHandle>`:

| Async method | Callback result |
| --- | --- |
| `SendMessage` | `Result<SendMessageResponse>` |
| `GetTask` | `Result<Task>` |
| `ListTasks` | `Result<client::ListTasksResponse>` |
| `CancelTask` | `Result<Task>` |
| `CreateTaskPushNotificationConfig` | `Result<TaskPushNotificationConfig>` |
| `GetTaskPushNotificationConfig` | `Result<TaskPushNotificationConfig>` |
| `ListTaskPushNotificationConfigs` | `Result<ListTaskPushNotificationConfigsResponse>` |
| `DeleteTaskPushNotificationConfig` | `Result<void>` |

The protobuf types in this table are in `lf::a2a::v1`. Existing synchronous methods
and `StreamObserver` remain available.

```cpp
lf::a2a::v1::GetTaskRequest request;
request.set_id("my-task");
a2a::client::CallOptions options;
options.timeout = std::chrono::seconds(5);

auto operation = client.Async().GetTask(request,
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
network cancellation of this outbound call. Use `CancelTask` to request a
task state transition. `Cancel()` does not wait for the callback. `IsActive()`
becomes false once a terminal result has been selected. `Wait()` waits for the callback
to return when called outside completion workers; inside an SDK callback it is a
no-op to prevent callback cycles. It is an observation aid, not another result
channel. Use callback delivery to access the result.

`AsyncClient` is a copyable, movable view of the owner's transport, interceptors,
and shutdown state. Retaining a view does not keep a destroyed `A2AClient` open;
all views reject new calls after owner shutdown. Reassign a moved-from view before
using it. An owning `A2AClient` and each
`AsyncClientTransport` have stable identities and cannot be copied or moved.
Concrete synchronous transport wrappers follow the Rule of Zero; copying a wrapper
shares its async component and native channel/client. Explicit shutdown closes that
shared component. A retained low-level `transport.Async()` pointer is itself an
owner of the async component; releasing its last owner drains outstanding calls.

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

One interceptor snapshot belongs to each call, sync or async. Hooks run without
holding the registration mutex, so registering another interceptor from a hook
affects subsequent calls and never creates an unmatched `AfterCall`. `BeforeCall` runs in
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
The default `ClientTransport::Async()` returns null; async initiation then
rejects with a validation error, without calling a synchronous operation. Custom
transports compose a separate `AsyncClientTransport` and return it from `Async()`.
Include `a2a/client/async_transport.h` to implement its protected `StartSendMessage`,
`StartGetTask`, and other `Start*` hooks using `core::AsyncCompletion<T>`. Copy request data needed after return,
register a cancellation signal with `RegisterCancellation`, and call `Complete`
with the final `Result<T>`. The token isolates callbacks and enforces once-only
completion; duplicate or late completions are ignored. Override
`DefaultAsyncTimeout()` when needed. Custom transport shutdown must close its
owned `async_scope_` before releasing network resources.

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
Sync mode uses one application caller per concurrency slot; async mode starts
replacements from callbacks. Measurements include callback dispatch and periodic resource sampling.
Do not compare these numbers to streaming or server coroutine benchmarks.

### Local comparison

A Linux x86-64 run used GCC 14, Release builds, libcurl 8.14, gRPC 1.51,
1024 loopback `ListTasks` calls per case, and concurrency 1, 8 and 32. The SUT
was freshly started with an empty in-memory task store for this comparison. The same candidate executable ran both modes;
compilation and linting were paused during measurement. These short local runs
illustrate dispatch costs and thread use, rather than predicting deployment
capacity. All 18 network cases completed without errors.

At concurrency 32:

| Binding | Mode | Requests/s | p50 ms | p95 ms | Peak sampled threads | Peak sampled RSS MiB |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| gRPC | Sync | 14893 | 1.927 | 3.065 | 40 | 17.4 |
| gRPC | Async | 14137 | 2.100 | 3.541 | 15 | 17.7 |
| HTTP+JSON | Sync | 16058 | 1.283 | 2.670 | 37 | 15.2 |
| HTTP+JSON | Async | 17782 | 1.482 | 3.751 | 10 | 14.7 |
| JSON-RPC | Sync | 21488 | 1.150 | 3.215 | 37 | 16.3 |
| JSON-RPC | Async | 19007 | 1.372 | 3.953 | 10 | 15.4 |

At concurrency 8, sync/async throughput was 12366/10138 requests/s for gRPC,
18043/14289 for HTTP+JSON and 19768/13919 for JSON-RPC. At concurrency 1,
async dispatch increased p50 latency from 0.097 to 0.158 ms for gRPC,
0.091 to 0.132 ms for HTTP+JSON and 0.127 to 0.147 ms for JSON-RPC. The shared
completion pool adds fixed overhead at low concurrency. Async is useful for
bounded application thread use; it does not guarantee higher throughput.
Thread counts include application and native runtime threads. RSS was sampled
externally from `/proc` every 5 ms and is approximate.

The unchanged synchronous parsing benchmark was also built at release baseline
`377375c` and at this implementation with identical Release configurations.
Median CPU time over five samples of at least 0.1 seconds per case was:

| Benchmark | Tasks | Baseline microseconds | Candidate microseconds | Change |
| --- | ---: | ---: | ---: | ---: |
| HTTP+JSON ListTasks | 1 | 4.691 | 4.643 | -1.0% |
| HTTP+JSON ListTasks | 20 | 65.536 | 64.279 | -1.9% |
| HTTP+JSON ListTasks | 200 | 659.550 | 679.125 | +3.0% |
| JSON-RPC ListTasks | 1 | 9.320 | 9.626 | +3.3% |
| JSON-RPC ListTasks | 20 | 74.198 | 72.289 | -2.6% |
| JSON-RPC ListTasks | 200 | 681.697 | 651.229 | -4.5% |

All six cases remain below the repository's absolute benchmark thresholds.
Changes range from -4.5% to +3.3%; these short measurements include noise and do
not establish a regression bound.

## Implementation choices

Sync and async client facades compose one private session for transport ownership,
interceptor registration and shutdown. Public async transport declarations live in
`async_transport.h`; each binding composes a private async implementation in a
separate source file. HTTP request construction and protocol decoders are shared,
as are gRPC metadata/deadline construction and error mapping. Both HTTP facades
retain the same native client and reactor connection cache.

The completion scope uses standard C++ mutexes, condition variables and threads.
Boost.Asio's executors, `thread_pool`, `steady_timer` and cancellation slots were
considered. Boost is an optional dependency behind `A2A_ENABLE_ASIO` for server
execution; using these types in core would make it mandatory for all clients.
They would also leave the SDK-specific once-only result claim, admission bound,
native cancellation bridge and draining/reentrant shutdown rules to implement.
The small transport-independent completion layer therefore remains based on the
standard library; native I/O stays with libcurl and gRPC. This does not add an Asio
I/O context or another network reactor.
