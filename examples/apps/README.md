# Example apps

All apps are deterministic and avoid external services unless an app README states otherwise. CI builds and runs every app in this directory through `scripts/run_examples.sh`.

- `hello_agent`: smallest end-to-end in-process client/server example.
- `simple_client`: client-side request construction and task fetch flow.
- `rest_server`: REST server transport setup.
- `json_rpc_server`: JSON-RPC server transport setup.
- `grpc_server`: gRPC service object setup.
- `streaming_client`: client-side streaming consumption.
- `streaming_server`: server-side stream session behavior.
- `push_notifications`: push notification configuration and delivery abstraction.
- `auth_policy_server`: auth metadata handling shape.