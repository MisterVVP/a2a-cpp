# Coroutine server streaming

Build with `A2A_ENABLE_ASIO=ON`, link `a2a::asio`, and register
`AsyncGrpcServerTransport` directly or through `ServerBindings`. The same optional
service supports unary calls, `SendStreamingMessage`, and `SubscribeToTask`.
Existing synchronous executors, sessions, transports, and registrations remain
available without Boost.

## Application contract

Override the streaming methods on `CoroutineAgentExecutor`. Their requests,
`RequestExecutionContext`, and shared `AsyncServerStreamSession` are owned by the
operation. Produce events using one completion model: await a write, then return
an application result when the stream ends.

```cpp
boost::asio::awaitable<a2a::core::Result<void>> SendStreamingMessage(
    lf::a2a::v1::SendMessageRequest request,
    a2a::server::engine::RequestExecutionContext context,
    std::shared_ptr<a2a::server::engine::AsyncServerStreamSession> stream) override {
  lf::a2a::v1::StreamResponse event;
  event.mutable_message()->set_task_id(request.message().task_id());
  event.mutable_message()->add_parts()->set_text("Streaming response");
  co_return co_await stream->Write(std::move(event));
}
```

Each `Write` takes ownership of an event. Await it before submitting another.
A successful write acknowledges transport consumption; it does not establish that
the remote application processed the event. The gRPC implementation retains the
protobuf until `OnWriteDone` and allows one in-flight write. A concurrent write is
rejected. Always propagate write failures. The dispatcher retains the first write
or authorization error even if an application mistakenly ignores it.

Returning success ends the stream normally. Returning an error or throwing ends
it with the existing error/status mapping. There is no separate application
`Finish` call. Unimplemented streaming methods return unsupported-operation
errors. An executor that serves only unary calls requires no changes.

The runnable `examples/apps/async_grpc_server` example includes a finite coroutine
stream. Its smoke mode and installed-package consumer test execute a real stream.

## Lifecycle and security

Streaming uses the [request lifecycle](request-execution.md) from the unary
engine. No second cancellation context is introduced. Metadata, authentication,
version and required-extension validation share the existing dispatch and gRPC
helpers. Task subscriptions preserve the request's tenant. With a security policy,
the dispatcher checks current stored task visibility before every event and
preserves the authenticated principal through interceptor updates.

Before hooks run before application execution. After hooks run once at terminal
completion, in reverse registration order. Streaming completion supplies an empty
`DispatchResponse`; events travel through the stream session. The dispatcher
reserves active admission for the whole stream. Logical cancellation releases
that admission; pending capacity remains reserved until coroutine cleanup ends.

`OnCancel` queues cancellation onto Asio. Pending writes and publication waits
wake through Asio cancellation, and the first terminal lifecycle transition wins.
Cancellation remains cooperative: an application that disables cancellation or
awaits a foreign backend must eventually release its resources. No application
code executes on the reactor thread. The reactor defers `Finish` until its
outstanding write completes, calls it once, and deletes itself in `OnDone`.
When cancellation interrupts a flow-controlled write, it cancels the native call
to release that write callback. Shutdown with an outstanding write can therefore
produce gRPC `CANCELLED` on the wire while the owned application context retains
`kServerShutdown`. Idle streams retain the `UNAVAILABLE` shutdown mapping.

Call `bindings.Shutdown()` (or the transport's `Shutdown()`) before shutting down
the gRPC server. Keep transport registrations alive through server `Wait()` and
drain the runtime before destroying application resources. This preserves the
`kServerShutdown` outcome instead of classifying host shutdown as client
cancellation. See [runtime ownership](runtime-helpers.md).

## Shared task publication

Implement `SubscribeTask` by obtaining an authorized task snapshot and awaiting:

```cpp
co_return co_await a2a::server::engine::AsyncTaskSubscription::Run(
    subscriptions, std::move(task), context, std::move(stream));
```

`subscriptions` is the same `TaskSubscriptionService` used by synchronous
`Subscribe` calls and `PublishTaskUpdated`. Keep it alive through subscription
startup. The operation then owns its registration state. Both subscription types
use the same current-task snapshot, latest published status, publication order,
and terminal-state rules. The first event is a task snapshot without history or
artifacts; subsequent events are status updates. Terminal tasks reject new
subscriptions. Terminal updates drain in order and end the stream.

Each async subscription has a bounded event buffer (64 by default; supply a
positive final `capacity` argument to change it), plus at most one event awaiting
a transport write. A full buffer fails that subscriber explicitly. Events are
not silently dropped or coalesced. Publishers never wait for transport demand,
and another subscriber's queue remains independent. The synchronous subscription
buffering contract is unchanged. Service shutdown wakes idle async subscribers
with an error; transport shutdown provides the canonical request shutdown reason.

The async path neither calls blocking `ServerStreamSession::Next` nor creates
watcher, polling, or pump threads. It suspends on publication notifications and
write acknowledgements. The async TCK fixture uses fixed backend workers for its
existing blocking startup/store operations, then delivers live updates through
this publication path. This does not provide asynchronous database I/O.

## Concurrency and latency evidence

Build `async_stream_execution_evidence` in the desired configuration, then run:

```sh
./build/tests/async_stream_execution_evidence 64
```

The executable compares synchronous gRPC and coroutine gRPC subscriptions using
the same shared publication implementation, metadata, initial snapshot and
terminal event. Both modes use sequential synchronous client reads over one
channel, so the client adds no thread per stream. It opens every subscription and
reads its first event before publishing the terminal update. There is no sleep or
polling in the async stream path.

JSON output reports zero executable errors, application execution threads, Linux
process thread counts before/pending, p95 first-event latency, and p95 completion
latency measured from terminal publication to client completion. Completion
latencies include sequential client draining and final-status receipt. Non-Linux
process thread counts are null. gRPC's shared runtime pools can affect total
process counts independently of SDK workers.

The smoke test checks successful delivery and a fixed async worker bound. The
p95 completion target is **10 ms** under an equivalent workload; latency is
reported rather than enforced as a host-independent test assertion. Record the
build configuration, toolchain and actual results when assessing that target.
HTTP/SSE socket scheduling remains #270; this change adds no HTTP operation routes.

A local Linux Debug comparison using GCC 15.3, gRPC 1.51.1 and Boost 1.92.0
produced the following c64 results. Both modes delivered two events per stream
with zero errors on the same three-CPU environment:

| Mode | Application threads | Process threads before → pending | First-event p95 | Completion p95 |
| --- | ---: | ---: | ---: | ---: |
| Synchronous | 64 | 9 → 73 | 0.35 ms | 7.78 ms |
| Coroutine | 2 | 12 → 12 | 0.33 ms | 2.71 ms |

The coroutine result meets the 10 ms completion target for this workload. These
single-run measurements include shared-runner scheduling and sequential client
draining; they do not establish a general latency guarantee. Repeat the command
with an optimized build when assessing deployment performance.
