# Event-driven HTTP server

`a2a::server::EventHttpServer` is an opt-in listener in `a2a::asio`. Build with
`-DA2A_ENABLE_ASIO=ON`, using the repository's pinned Boost installation. Normal
SDK builds retain their existing dependency set and synchronous APIs.

```cpp
#include <a2a/server/engine/async_runtime.h>
#include <a2a/server/event_http_dispatcher.h>
#include <stdexcept>
#include <string>

// executor implements CoroutineAgentExecutor; card describes your agent.
a2a::server::engine::AsyncRuntime application(executor);
a2a::server::EventHttpServerOptions limits;
limits.port = 8080;
a2a::server::EventHttpServer listener(
    a2a::server::MakeEventHttpHandler(application.dispatcher(), card), limits);
auto started = listener.Start();
if (!started.ok()) {
    throw std::runtime_error(std::string(started.error().message()));
}
// ... application lifetime ...
listener.Shutdown();
listener.Join();
application.Shutdown();
application.Join();
```

The [installed-consumer example](../examples/apps.md) in
`examples/apps/event_http_server` builds without source-tree include paths. `MakeEventHttpHandler` owns its REST,
JSON-RPC and agent-card transports, routing mux, and dispatcher reference.
`ServerBindingsOptions` selects paths, extension requirements and discovery
cache settings. The same preparation and completion code implements synchronous
and asynchronous validation, protocol errors, request metadata, authentication,
agent-card routes and response serialization. Root aliases the JSON-RPC path.

## Portable readiness design and dependency decision

The listener uses Boost.Asio's native TCP operations: epoll on Linux, kqueue on
macOS, and IOCP on Windows. Exactly one thread runs network completions. It owns
all sockets, connection state, parser buffers, output queues and timers. Blocking
accept/read/write, per-connection threads, and sleep-based readiness polling are
absent from this runtime. Partial socket writes continue through `async_write`.

Alternatives considered were a new libuv dependency, standalone Asio, and a
handwritten epoll/kqueue/IOCP abstraction. Reusing the SDK's existing optional
Boost.Asio target avoids another dependency and duplicated platform lifecycle
code. Windows links Winsock and Mswsock only on the optional target. Installed
users of the optional SDK already resolve the pinned Boost headers; users of
default builds do not require Boost. There is no change to outbound libcurl.

`HttpAdapter::ParseRequest` accepts supplied bytes, returns an empty optional
for incomplete input, and reports exactly one request's consumed byte count.
Its stateful overload caches parsed headers and resumes delimiter searches
across byte fragments, keeping framing work linear. Keep its input prefix until
success, then remove the consumed bytes before reusing the connection state.
`EncodeResponseHead` and `EncodeResponse` produce reusable bytes. The blocking
adapter shares these helpers. Content length, version and size validation,
fragmentation, connection-close tokens and pipeline boundaries remain common.
Request transfer encoding remains unsupported. Socket scheduling is independent
of this codec.

## Scheduling and ownership

The listener owns a fixed application `io_context` pool, separate from its
network thread. Every handler, stream producer and cancellation observer runs
off the network thread. Native dispatcher execution runs on the application's
own `AsyncRuntime` or supplied executor. Its existing admission and cancellation
contracts remain authoritative; the HTTP runtime does not implement another
application dispatcher.

An async response may place already available SSE bytes in `response.body`.
The listener sends this prefix before invoking the stream producer and combines
it with the headers when both fit the output bound. The producer writes only
the remaining events. If the combined write would exceed the bound, the prefix
is sent separately with the same write acknowledgement and cancellation rules.

A synchronous `Handler` may call existing REST/JSON-RPC transports or a mux.
Such callbacks must support concurrent invocation, or set
`application_threads = 1` to serialize legacy application work. Captured objects
must outlive `Join()`. Legacy `HttpServerResponse::stream_writer` callbacks are
rejected with HTTP 501: use the synchronous listener for those streams, or
migrate production to the async stream-session API. No waiting worker per
legacy live stream is silently introduced.

Each connection processes one application request at a time. Further pipelined
bytes remain in its bounded input buffer, preserving response and side-effect
ordering. Other connections can progress independently. Receive EOF ends request
input; active responses, including finite SSE, and buffered requests drain before
the connection closes. Resets, write failures, idle expiry and shutdown close the
socket and signal cancellation.

## Bounds and overload

| Option | Default | Policy |
| --- | --- | --- |
| `max_connections` | 1024 | Additional accepted sockets close immediately |
| `max_pending_requests` | 128 | Includes queued/running handlers and streams; HTTP 503 and close on exhaustion |
| `application_threads` | 4 | Fixed worker count; synchronous work can occupy at most this many workers |
| `max_input_bytes` | 1 MiB | Excess pipelined/fragmented input closes the connection |
| `codec.max_request_size` | 1 MiB | Bounds each request, including headers and body; malformed framing receives HTTP 400 and close |
| `max_output_bytes` | 1 MiB | Bounds queued socket output, including chunk overhead; overflow fails the write and closes the connection |
| `idle_timeout` | 30 s | Bounds idle or stalled socket lifetime |
| `heartbeat_interval` | 15 s | SSE comment when output is empty; zero disables it |

Application response construction and one event's serialization are owned by the
application. The transport validates encoded output against its byte limit;
applications should also bound event and response objects before constructing
them. Kernel socket buffers have operating-system limits in addition to these
userspace bounds. Configure input capacity for the largest allowed request and
the intended amount of pipelining. An oversized response or invalid response
framing closes the connection. Handler exceptions also close it.

There is no separate unbounded application submission queue: admission is taken
before posting work to the fixed pool and retained through stream completion or
handler cleanup. Native dispatcher admission also bounds coroutine cleanup after
logical cancellation. Across connections, memory is proportional to configured
connection and request limits, rather than elapsed connection lifetime.

## Finite and live SSE

`MakeEventHttpHandler` connects the existing `AsyncDispatcher::DispatchStream`
and `AsyncServerStreamSession::Write` contract to HTTP chunked output. REST and
JSON-RPC use their existing event/error serializers. Preparation errors and
application failures before the first event retain ordinary protocol error
responses. After streaming starts, errors use the binding's SSE error encoding.

The application awaits every event write. A capacity-one mailbox transfers the
first event for HTTP header selection. Subsequent writes await the socket sink
directly on the dispatcher strand. Every write is acknowledged only after the
corresponding socket write completes. This provides bounded demand without
pinning a worker during a slow read. Awaited writes preserve event order;
heartbeats never accumulate behind a stalled write. Finite completion writes a
final chunk and permits connection reuse. A request's `Connection: close` still
closes after its chunked response completes.

Disconnect and shutdown wake mailbox waits and socket writes, and call the
existing dispatcher's cancellation entrypoint. `RequestExecution` arbitrates
completion/cancellation races and runs application cleanup exactly once. Server
shutdown cancellation is distinguished from peer cancellation and idle expiry.
Native dispatcher cancellation also wakes a backpressured socket write without
requiring listener shutdown. Cancelling an in-flight write closes that connection
because a partially written chunk cannot be completed safely. Completion and
failure resume the awaiting coroutine on its own executor. No HTTP stream polling
thread is created.

Custom asynchronous handlers can return `EventHttpResponse::stream`, receiving
an owned `HttpEventStream` and `RequestExecutionContext`. Await each `Write` and
observe its result and the context's stop token. Returning from the producer
ends the stream. Custom producers must cooperate with cancellation, just as
native application coroutines do.

## Shutdown and unsupported configurations

Configure the listener before its single successful `Start()`. `port()` returns
the bound port, including when port zero requested an ephemeral port.
`Shutdown()` is thread-safe, wakes the network loop, closes admission and sockets,
and posts cancellation off the network thread. `Join()` and destruction drain
network/HTTP callbacks and release handler captures on application workers.
Call them outside runtime callbacks. Blocking legacy work must eventually
return, and custom producers must honor cancellation.
The dispatcher and its executor must stay alive until the listener has drained.
The application runtime's subsequent `Join()` drains native coroutine cleanup,
which may outlive logical cancellation while a backend finishes.
Retained stream sinks reject writes after stream completion, shutdown or
listener destruction; their closed socket/timer services retain owned context
storage until those application-held sinks are released.

The listener accepts HTTP/1.1 over TCP. For HTTPS or HTTP/2 clients, place a
reverse proxy in front of it and forward requests to the listener using
HTTP/1.1. Configure the proxy to forward SSE events promptly without buffering
and to allow long-lived responses. Upstream requests must use Content-Length
for their bodies. Native HTTP/2, TLS, request chunked transfer encoding, protocol
upgrades and legacy blocking stream producers are outside this API.

The async TCK SUT uses this same SDK runtime for HTTP+JSON, JSON-RPC, discovery,
and SSE. Its signal monitor and test diagnostics remain fixture concerns. The
synchronous SUT remains available as a compatibility baseline. Portable native
socket tests run in the existing Linux, macOS and Windows Asio CI configurations.

## Measuring resource use

`tests/performance/event_http_evidence.py` compares discovery, finite SSE and
64 idle sockets against the synchronous SUT at c1/c16/c64. Build both binaries
with identical optimized settings and dependencies, then follow the performance
README. The runner records throughput, completion and first-event p95/p99,
errors, CPU, resident memory and process threads. Short finite streams incur
additional executor scheduling and awaited write acknowledgements; use the
comparison to assess the tradeoff for your workload.
