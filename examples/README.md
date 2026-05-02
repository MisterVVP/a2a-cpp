# Examples

This folder provides runnable, in-process examples for the A2A C++ SDK.

## Example index

| Example | Target | Demonstrates | Run command | Expected output snippet |
|---|---|---|---|---|
| discovery_only_client.cpp | `example_discovery_only_client` | Agent-card discovery | `./build/examples/example_discovery_only_client` | `discovered agent:` |
| rest_client.cpp | `example_rest_client` | REST `SendMessage` | `./build/examples/example_rest_client` | `created task:` |
| json_rpc_client.cpp | `example_json_rpc_client` | JSON-RPC `SendMessage` | `./build/examples/example_json_rpc_client` | `json-rpc created task:` |
| streaming_client.cpp | `example_streaming_client` | SSE `SendStreamingMessage` | `./build/examples/example_streaming_client` | `status event:` |
| grpc_client.cpp | `example_grpc_client` | gRPC `GetTask` | `./build/examples/example_grpc_client` | `Task id:` |
| list_tasks_client.cpp | `example_list_tasks_client` | REST `ListTasks` | `./build/examples/example_list_tasks_client` | `listed tasks:` |
| cancel_task_client.cpp | `example_cancel_task_client` | REST `CancelTask` | `./build/examples/example_cancel_task_client` | `task state after cancel:` |
| push_notification_config_client.cpp | `example_push_notification_config_client` | Push-config CRUD/list APIs | `./build/examples/example_push_notification_config_client` | `push configs:` |
| interceptor_client.cpp | `example_interceptor_client` | Client interceptor before/after hooks | `./build/examples/example_interceptor_client` | `before GetTask` |
| auth_policy_server.cpp | `example_auth_policy_server` | Server auth metadata extraction + policy point | `./build/examples/example_auth_policy_server` | `missing auth status:` |
| minimal_server_custom_executor.cpp | `example_minimal_server_custom_executor` | Minimal server setup | `./build/examples/example_minimal_server_custom_executor` | `agent-card status:` |

## Build all examples

```bash
cmake -S . -B build -DA2A_BUILD_EXAMPLES=ON
cmake --build build --parallel
```

## Run all examples sequentially

```bash
./scripts/run_examples.sh
```

See also: `docs/quickstart.md` for first-run setup and `docs/client-usage.md`/`docs/server-usage.md` for API details.
