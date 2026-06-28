# Discovery

Discovery resolves where and how to call an A2A agent.

## Happy path

1. Use `DiscoveryClient` to fetch an `AgentCard`.
2. Use `AgentCardResolver` to select an endpoint and transport.
3. Use resolved endpoint metadata to construct your client transport.

## Operational context

- Cache discovery results where practical, with clear refresh strategy.
- Handle missing/partial card metadata defensively.
- Prefer explicit fallback order when multiple transports are available.

## Failure scenarios

- Discovery endpoint unavailable.
- Invalid or incomplete `AgentCard`.
- Unsupported transport in discovered card.

## Example

See `examples/apps/simple_client/main.cpp` for a minimal discovery flow.
