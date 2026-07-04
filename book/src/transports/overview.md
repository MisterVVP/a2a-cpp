# Transports Overview

The SDK separates protocol operations from transport adapters. Applications choose the transport that best matches their deployment boundary and interoperability needs.

## Available transports

- [REST](rest.md): HTTP+JSON resource-oriented integration for clients and servers.
- [JSON-RPC](json-rpc.md): JSON-RPC 2.0 method dispatch over HTTP-style request handling.
- [gRPC](grpc.md): protobuf/gRPC service integration with unary and streaming RPCs.

## Choosing a transport

- Choose REST when your platform already standardizes on HTTP routing, gateways, or resource-style APIs.
- Choose JSON-RPC when method-style dispatch is easier to interoperate with or proxy.
- Choose gRPC when you want protobuf-native contracts, gRPC streaming, or a service mesh that already supports gRPC well.

## Shared operational concerns

Regardless of transport, define these policies explicitly:

- Authentication and authorization boundaries.
- Protocol version and required-extension handling.
- Request deadlines and payload limits.
- Retry and idempotency behavior.
- Telemetry, audit logs, and stable task/request identifiers.
