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

## Operational guidance

- Use bounded polling with backoff when not using streaming.
- Enforce authorization checks on server-side task visibility.
- Avoid exposing full task history to clients that do not need it.
