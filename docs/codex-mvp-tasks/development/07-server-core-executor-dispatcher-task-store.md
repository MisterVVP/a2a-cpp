# Task 07 — Server core: executor interface, dispatcher, and task store

## Goal

Create the transport-agnostic server core so users can implement an A2A agent once and expose it via multiple transports later.

## Scope

- Define `AgentExecutor` interface for server-side business logic.
- Define request context and response helpers.
- Build a transport-agnostic dispatcher that routes incoming A2A operations to the executor.
- Add an in-memory task store abstraction with a pluggable interface.
- Support lifecycle operations needed by REST and JSON-RPC server transports later.

## Constraints

- A2A **1.0 only**.
- Keep executor independent from HTTP / JSON-RPC details.
- Do not implement durable persistence yet.
- Do not implement auth policy decisions here; only pass context through.

## Deliverables

- `AgentExecutor` interface
- dispatcher
- task store interface + in-memory implementation
- unit tests for dispatch and task operations

## Suggested API shape

```cpp
class AgentExecutor {
public:
  virtual Result<a2a::SendMessageResponse> SendMessage(
      const a2a::SendMessageRequest&, RequestContext&) = 0;

  virtual std::unique_ptr<ServerStreamSession> SendStreamingMessage(
      const a2a::SendMessageRequest&, RequestContext&) = 0;

  virtual Result<a2a::Task> GetTask(
      const a2a::GetTaskRequest&, RequestContext&) = 0;

  virtual Result<a2a::ListTasksResponse> ListTasks(
      const a2a::ListTasksRequest&, RequestContext&) = 0;

  virtual Result<a2a::Task> CancelTask(
      const a2a::CancelTaskRequest&, RequestContext&) = 0;

  virtual ~AgentExecutor() = default;
};
```

## Implementation notes

- The dispatcher should convert transport-neutral input into executor calls.
- Request context should carry:
  - request ID if available
  - auth metadata
  - client headers
  - remote address if available
- The task store should support enough operations for:
  - create/update/get/list/cancel
  - stream subscription hooks later

## Test expectations

Cover:
- successful dispatch to each implemented method
- unsupported method
- executor returning validation or remote errors
- task store happy path and basic concurrency assumptions if relevant

## Acceptance criteria

- An SDK user can implement a mock executor and handle core A2A methods without knowing the transport.
- Task store supports core task lifecycle operations needed by later transports.
- Dispatcher behavior is deterministic and well-tested.

## Out of scope

- REST endpoint exposure
- JSON-RPC endpoint exposure
- gRPC server
