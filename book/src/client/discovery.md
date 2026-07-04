# Discovery and Agent Cards

Discovery resolves server capabilities before constructing a client transport.

## Public and extended cards

`DiscoveryClient` supports:

- `Fetch(base_url)` for the standard Agent Card.
- `FetchExtendedAgentCard(base_url)` for the extended Agent Card endpoint.

Fetched cards are cached for `kDefaultDiscoveryCacheTtl` (300 seconds) unless a different TTL is supplied.

## Interface resolution

`AgentCardResolver::SelectPreferredInterface(card, preferred)` selects a `ResolvedInterface` for one of:

- `PreferredTransport::kRest`
- `PreferredTransport::kJsonRpc`
- `PreferredTransport::kGrpc`

The result includes the transport, URL, security requirements, and security schemes needed to configure a client.

## Operational guidance

- Validate and log the selected endpoint during startup.
- Prefer an explicit fallback order when cards advertise multiple transports.
- Treat discovery metadata as untrusted input until validated.
- Refresh cached cards after deployment or capability changes.

## Related files

- `include/a2a/client/discovery.h`
- `include/a2a/core/agent_card/agent_card_provider.h`
- `include/a2a/server/agent_card/agent_card_serializer.h`
