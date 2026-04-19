# Task 04 — HTTP+JSON client for core A2A RPCs

## Goal

Implement the first production-usable client transport: HTTP+JSON/REST for the core non-streaming A2A 1.0 operations.

## Scope

Implement client support for:
- `SendMessage`
- `GetTask`
- `ListTasks`
- `CancelTask`
- push notification config CRUD if the endpoints are available in the official 1.0 mapping
- `GetExtendedAgentCard` if mapped via REST in the spec / examples

Use the discovered Agent Card and resolved interface from Task 03.

## Constraints

- A2A **1.0 only**.
- Reuse the shared ProtoJSON helpers.
- Keep transport-independent request/response shapes in generated protobuf types.
- Keep HTTP concerns isolated in the transport layer.

## Deliverables

- `HttpJsonTransport`
- high-level `A2AClient` or equivalent wrapper for non-streaming methods
- request building and response parsing
- HTTP error mapping to SDK errors
- integration tests with a fake or fixture-backed server

## Suggested file layout

```text
include/a2a/client/client.h
include/a2a/client/call_options.h
src/client/client.cpp
src/client/http_json/http_json_transport.cpp
src/client/http_json/http_json_transport.h
tests/integration/http_json_client_*.cpp
```

## Implementation notes

- Always send `A2A-Version: 1.0`.
- Support optional `A2A-Extensions`.
- Support configurable headers, timeouts, and auth hooks.
- Parse JSON responses into generated protobuf messages.
- Centralize endpoint construction rather than hardcoding paths in multiple places.
- Keep room for retry policy, but do not add automatic retries unless clearly safe.

## Suggested public API shape

```cpp
class A2AClient {
public:
  Result<a2a::SendMessageResponse> SendMessage(
      const a2a::SendMessageRequest&, const CallOptions&);
  Result<a2a::Task> GetTask(
      const a2a::GetTaskRequest&, const CallOptions&);
  Result<a2a::ListTasksResponse> ListTasks(
      const a2a::ListTasksRequest&, const CallOptions&);
  Result<a2a::Task> CancelTask(
      const a2a::CancelTaskRequest&, const CallOptions&);
};
```

## Test expectations

Add coverage for:
- happy-path message send
- task fetch
- task list
- cancel
- remote 4xx and 5xx
- invalid JSON body
- unsupported version response
- missing endpoint mapping from Agent Card

## Acceptance criteria

- A high-level client can execute core non-streaming A2A calls over REST.
- Headers are set correctly.
- Responses round-trip through generated protobuf types.
- Errors preserve both HTTP and protocol context when possible.

## Out of scope

- SSE streaming
- JSON-RPC transport
- server runtime
