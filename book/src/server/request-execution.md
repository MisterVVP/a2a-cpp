# Request execution lifetime

`a2a/server/engine/request_execution.h` provides the transport-independent lifecycle
contract for opt-in asynchronous unary execution. All request execution types live
in the `a2a::server::engine` namespace. This foundation is usable on its
own; executor, dispatcher and transport integration follow in later changes.

## Availability

The lifecycle API requires a standard library providing C++20 `std::stop_token`,
`std::stop_source` and `std::stop_callback`. CMake probes this capability and exports
`A2A_HAS_REQUEST_EXECUTION` as `1` or `0` through `a2a::server`. Consumers can use
that definition to conditionally include this header and use the API. When the
capability is absent, the new implementation and tests are omitted; existing
synchronous SDK targets remain available. Experimental standard library flags are
not enabled automatically.

## Ownership and metadata

Create one `RequestExecution` for each operation and pass `Context()` to application
work. `RequestExecutionContext` is a copyable, shared view of immutable metadata,
an optional `std::chrono::steady_clock` deadline, and the operation's outcome.
`Metadata()` preserves all `RequestContext` fields, including auth metadata,
headers, principal, request ID and protocol version. The constructor takes a
snapshot; later changes to the original metadata do not affect the operation.
Metadata remains untrusted until validated by the application.

The execution owner is neither copyable nor movable. Retain it until a terminal
signal has been handled. If it is destroyed while pending, destruction publishes
`kServerShutdown` and requests cancellation. Application contexts and stop tokens
remain valid after the owner or transport is destroyed. References returned by
`Metadata()` require a live context copy. As with ordinary C++ objects, callers
must keep the owner alive during concurrent member calls. An inline cancellation
observer may release the owner once no other caller can access it.

## Terminal outcomes and races

An execution starts at `kPending`. Exactly one atomic transition selects its
terminal outcome; subsequent signals return false and cannot replace that outcome.

| Signal | Outcome | Stop requested |
| --- | --- | --- |
| `TryComplete(kSucceeded)` | `kSucceeded` | No |
| `TryComplete(kFailed)` | `kFailed` | No |
| `Cancel(kClientCancelled)` (the default) | `kClientCancelled` | Yes |
| `ExpireDeadline()` or `Cancel(kDeadlineExceeded)` | `kDeadlineExceeded` | Yes |
| `Cancel(kServerShutdown)` or pending owner destruction | `kServerShutdown` | Yes |

Application failure is distinct from cancellation. `TryComplete` chooses the
lifecycle outcome; response values and structured application errors belong to
the caller. The caller must publish a result only if `TryComplete` returns true.
The lifecycle does not invoke an application completion callback.

Concurrent completion and cancellation use first-terminal-transition precedence.
A completion accepted before the deadline stays accepted even if a deadline or
cancellation arrives later. Cancellation never waits for application work to
finish and does not forcefully interrupt it. Releasing application resources and
joining work remain the caller's responsibility.

## Deadlines and scheduling

No SDK timer, worker, or per-request thread is created. A deadline is an absolute
monotonic time; a transport must convert its deadline to this clock at ingress.
The owner must arrange for its scheduler to call `ExpireDeadline` at/after that
time if prompt notification is required. Reads of `Deadline()` and `Outcome()`
do not run timers or change state. Even an already expired deadline remains
pending until an expiry signal or completion check.

`TryComplete` checks the deadline before attempting application completion. At
exactly the deadline or later, it attempts to publish `kDeadlineExceeded` and
returns false for either success or failure. `ExpireDeadline` returns false
before the deadline or if no deadline exists. A transport can also explicitly
report expiry via `Cancel(kDeadlineExceeded)`. All cancellation reasons compete
under the same first-terminal-transition rule. The optional `now` argument on
completion and expiry allows schedulers and tests to supply a clock sample.

## Cancellation observers

Use `context.StopToken()` with C++20 `std::stop_callback`, or poll
`stop_requested()` at cooperative cancellation points. The terminal reason is
published before observers are notified, so callbacks can inspect `Outcome()`.
Outcome publication and stop notification are separate steps: a concurrent read
may observe a cancellation outcome just before the stop token is signalled.
After the winning cancellation call returns, stop has been requested.

Each registered callback runs at most once. A registration after cancellation
runs immediately during registration. Destroying a registration prevents future
invocation and waits for a callback already executing on another thread, according
to the standard `std::stop_callback` lifetime rules. Keep every captured resource
alive until registration destruction finishes. Retaining a registration after
successful completion is safe; success and failure do not notify it.

Callbacks execute inline on the thread requesting cancellation (including owner
destruction), or on the registering thread for late registration. They must be
short, non-blocking and non-throwing; enqueue application cleanup on an appropriate
scheduler when necessary. No SDK mutex is held while observers run. Observer
callbacks may inspect the context or attempt a competing terminal transition.
A slow observer delays the winning cancellation call; losing terminal signals do
not wait for observer completion. Observer order is unspecified.
