# Task 05 — SSE streaming client for SendStreamingMessage and task subscription

## Goal

Add streaming support over SSE for A2A 1.0 so the SDK can consume incremental responses from agents.

## Scope

Implement:
- `SendStreamingMessage`
- task subscription streaming
- SSE parser
- stream observer / callback API
- cancellation and clean shutdown behavior

The stream payloads should map into generated A2A stream response messages.

## Constraints

- A2A **1.0 only**.
- Keep the SSE parser robust to chunking and partial reads.
- Do not implement speculative reconnect logic unless clearly defined and safe.
- Keep API thread-safety expectations documented.

## Deliverables

- SSE parser component
- streaming transport integration in HTTP client
- `StreamObserver` or callback-based public API
- tests for fragmented frames and normal stream sequences

## Suggested API shape

```cpp
class StreamObserver {
public:
  virtual void OnEvent(const a2a::StreamResponse&) = 0;
  virtual void OnError(const Error&) = 0;
  virtual void OnCompleted() = 0;
  virtual ~StreamObserver() = default;
};

class StreamHandle {
public:
  void Cancel();
  bool IsActive() const;
};
```

## Implementation notes

- Parse standard SSE framing correctly:
  - `event:`
  - `data:`
  - empty-line event termination
- Handle multi-line `data:` blocks.
- Support stream shutdown on server completion and local cancellation.
- Distinguish parse failure from remote failure event where possible.
- Build a deterministic fake stream source for testing.

## Test expectations

Cover:
- simple event sequence
- chunked / fragmented event frames
- multi-line data
- malformed frame
- cancel during active stream
- remote close without terminal event
- observer receiving task / status / artifact updates in order

## Acceptance criteria

- Client can consume streaming responses without buffering the full response.
- Parser handles partial network reads correctly.
- API surfaces stream completion, cancellation, and errors clearly.
- Tests cover both normal and broken stream conditions.

## Out of scope

- JSON-RPC
- server-side streaming endpoints
