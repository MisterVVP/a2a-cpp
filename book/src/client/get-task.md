# Get Task and List Tasks

`GetTask` retrieves a single task by ID. `ListTasks` returns a paginated list of tasks through the client abstraction and supported transports.

## GetTask flow

1. Store the task ID from `SendMessage` or a stream event.
2. Build `lf::a2a::v1::GetTaskRequest`.
3. Call `A2AClient::GetTask`.
4. Interpret task status, artifacts, and history according to your application policy.

## ListTasks flow

`a2a::client::ListTasksRequest` contains:

- `page_size`
- `page_token`

The response includes task values plus `next_page_token`.

An omitted page size requests the protocol default of at most 50 tasks. Explicit
page sizes must be between 1 and 100. Continue until `next_page_token` is empty:

```cpp
a2a::client::ListTasksRequest request{.page_size = 50};
do {
  const auto page = client.ListTasks(request);
  if (!page.ok()) {
    return page.error();
  }
  for (const auto& task : page.value().tasks) {
    ProcessTask(task);
  }
  request.page_token = page.value().next_page_token;
} while (!request.page_token.empty());
```

Keep pages bounded (normally no more than 100 tasks) and request a small
`history_length`, often 0 or 1, unless deeper history is required. CPU cost grows
with both the returned task count and retained history. Oversized results also
increase protobuf allocations and copies, response memory, serialized payload
size, transport latency, and timeout risk. In-memory stores hold their shared
read lock longer while materializing a result, so writes may wait longer. Full
history projection can dominate the list operation.

## Operational guidance

- Use bounded polling with backoff when not using streaming.
- Enforce authorization checks on server-side task visibility.
- Avoid exposing full task history to clients that do not need it.
