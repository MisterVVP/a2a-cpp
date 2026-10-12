# A2A protocol version handling

A2A protocol versions identify wire semantics and are independent of SDK release
versions. This SDK release implements A2A **1.0**; an agent's release/version
field is also separate from each interface's `protocolVersion`.

Negotiation matches **Major.Minor**. Numeric patch numbers are ignored by clients
and servers (`1.0.42` selects `1.0`). Servers reject malformed versions and
unsupported major or minor versions with `VersionNotSupportedError`. As required by specification
§3.6, an empty or omitted version denotes **0.3**. Since this SDK does not
implement 0.3 semantics, it rejects that fallback, including in the SUT. It never
silently treats an empty version as 1.0.

Public C++ SDK clients send `A2A-Version` on every unary operation, push CRUD,
streaming send, and subscription. gRPC sends the lowercase `a2a-version` metadata
key. The default is `1.0`. Request a version explicitly using `CallOptions`:

```cpp
a2a::client::CallOptions options;
options.protocol_version = "1.0.42";  // SDK emits A2A-Version: 1.0.
auto result = client.GetTask(request, options);
```

The explicit option determines the version, even if arbitrary headers or an
authentication hook supplies a conflicting version header. SDK clients remove
case-insensitive header aliases and emit exactly one canonical parameter after
credentials are applied. They reject malformed versions before sending a request.
An explicit empty option sends an empty value to exercise the 0.3 fallback.
Unsupported but syntactically valid versions are sent to the server for negotiation.

SDK server adapters validate versions before dispatch, then place the selected
Major.Minor in `RequestContext::protocol_version`. HTTP+JSON, JSON-RPC and gRPC
use the same SDK validator. The legacy `require_version_header=false` setting
changes the missing-header diagnostic; it does not bypass the required 0.3
fallback. Wire headers/metadata remain available unchanged in `client_headers`.

| Requirement | Independent SDK coverage |
|---|---|
| `VER-CLIENT-001` | All eight unary/push methods, streaming send and subscription emit the version parameter on every binding |
| `VER-CLIENT-002` | Requested `1.0.42` emits `1.0`; raw wire patch versions select the same 1.0 semantics; conflicting header aliases/hooks are removed |
| `VER-SERVER-001` | Accepted requests dispatch with 1.0 semantics in `RequestContext`, independently of SUT handlers |
| `VER-SERVER-002` | Unsupported versions never dispatch and return JSON-RPC `-32009`, HTTP 400, or gRPC `UNIMPLEMENTED` with protocol-code metadata |
| `VER-SERVER-003` | Empty 0.3 fallback is rejected before dispatch on every operation and binding |

Core/version-validator unit tests cover malformed components, patch handling,
case-insensitive lookup and empty/missing versions. `protocol_version_integration_test`
constructs SDK adapters and dispatch independently of the SUT, records actual HTTP
headers or real gRPC metadata, and identifies the operation and binding on failure.
Live SUT version scenarios run with both storage backends.

```bash
ctest --test-dir build -L protocol_version --output-on-failure
ctest --test-dir build -R 'CoreVersionTest|VersionHeaderValidatorTest' --output-on-failure
```

## TCK evidence and the Python SDK regression

[a2a-python issue #856](https://github.com/a2aproject/a2a-python/issues/856)
reported that unsupported `A2A-Version: 99.0` was accepted on JSON-RPC and
HTTP+JSON. The upstream `TestVersionErrors` reproduces this regression against
our live SUT. SDK validation rejects unsupported versions before dispatch using
the prescribed errors above. Empty/missing headers select 0.3; a 1.0-only server
must reject that unsupported version rather than silently accept it as 1.0.

The [mandatory TCK flow](README.md) executes the independent SDK checks and live
SUT scenarios through the supplemental C++ SDK test module, alongside upstream
tests. All five `VER-*` requirements must be **PASS on every binding**, with
executable SDK evidence and no skipped scenarios. XML and logs are retained in
the TCK report's `sdk-scenarios` directory. Missing coverage fails the report gate
even if pytest otherwise exits successfully. This gate verifies the five version
and four binding requirements; it does not imply full TCK compliance.
