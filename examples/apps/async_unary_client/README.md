# Async unary client

Build with the installed SDK using `A2A_EXAMPLE_APP=async_unary_client`, then run
`a2a_example http://localhost:50061/a2a` against a REST endpoint. The example starts
`Async().ListTasks` and receives its result on a shared completion worker.

`a2a_example --smoke` checks installed headers, linking and asynchronous error
completion against an unused loopback port. It also works with libcurl disabled.
See the [async unary guide](../../../book/src/client/async-unary.md) for cancellation,
shutdown, custom transport hooks and the gRPC/JSON-RPC equivalents.
