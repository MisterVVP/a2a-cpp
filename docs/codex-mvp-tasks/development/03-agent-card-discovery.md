# Task 03 — Agent Card discovery and interface resolution

## Goal

Implement A2A 1.0 discovery via `/.well-known/agent-card.json`, parse the returned Agent Card, validate required fields, and resolve the preferred interface for later client calls.

## Scope

- Fetch `/.well-known/agent-card.json` from a base URL.
- Parse the response into the generated Agent Card protobuf type.
- Validate key fields enough for SDK use.
- Resolve preferred interface from `supportedInterfaces`.
- Surface security requirements and schemes in a usable form.
- Add basic caching support for discovered Agent Cards.
- Add unit tests and at least one integration-style fixture test.

## Constraints

- A2A **1.0 only**.
- Do not implement all auth flows here; only parse and expose metadata.
- Use ProtoJSON helpers from Task 02.

## Deliverables

- `DiscoveryClient` or similar API
- `AgentCardResolver` utility
- lightweight validation helpers
- cache support with TTL or simple in-memory memoization
- tests with valid and invalid card fixtures

## Suggested public API

```cpp
class DiscoveryClient {
public:
  Result<a2a::AgentCard> Fetch(std::string_view base_url);
};

class AgentCardResolver {
public:
  Result<ResolvedInterface> SelectPreferredInterface(
      const a2a::AgentCard& card,
      PreferredTransport preferred);
};
```

## Validation expectations

At minimum validate:
- card is parseable
- supported interfaces are present when required
- URLs / endpoint fields needed by the chosen interface are available
- advertised version is compatible with 1.0 expectations
- security metadata is structurally usable

## Implementation notes

- Normalize base URLs carefully.
- Handle common failures:
  - 404 on well-known path
  - malformed JSON
  - valid JSON but invalid protocol shape
  - no usable interfaces
- Caching can be simple in-memory for now.
- Keep validation pragmatic: enough to protect the client from obvious bad inputs without creating a full schema engine.

## Acceptance criteria

- Given a valid Agent Card fixture, discovery returns a parsed protobuf object.
- Resolver can choose between at least REST, JSON-RPC, and gRPC when present.
- Invalid or unusable cards produce actionable SDK errors.
- Unit tests cover malformed URL, 404, bad JSON, and no supported interface cases.

## Out of scope

- message sending
- streaming
- server-side card publishing
