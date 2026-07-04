# REST Server Transport

`RestServerTransport` maps HTTP+JSON requests onto dispatcher operations.

## Responsibilities

- Validate HTTP method, path, and content type.
- Parse JSON request bodies into protobuf messages.
- Populate `RequestContext`, including auth metadata.
- Serialize protocol responses and errors back to HTTP responses.

## Supported operation shape

REST transport covers core task lifecycle operations, streaming-compatible endpoints where wired by the hosting environment, Agent Card routes, and push-notification config APIs exposed by the dispatcher/executor.

## Hosting guidance

The SDK provides protocol mapping, not a full production web server framework. In production, place it behind an HTTP runtime that owns TLS, connection limits, request-size limits, access logs, and graceful shutdown.

## Example

See `examples/apps/rest_server/main.cpp`.
