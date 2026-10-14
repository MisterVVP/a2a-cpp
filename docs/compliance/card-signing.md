# Signed Agent Cards

`SignAgentCard` signs public protobuf Agent Cards using an Ed25519 PEM private key
and a nonempty key ID. Cryptographic operations use OpenSSL. The protected JWS
header contains `alg: EdDSA`, `kid` and `typ: JOSE`; the payload excludes the entire
`signatures` field and uses RFC 8785 JSON Canonicalization Scheme (JCS).

Signing is optional and disabled by default. Enable `A2A_ENABLE_SIGNING=ON`
and link `a2a::signing`; this target requires OpenSSL 3.0 or newer. Core,
client and server builds do not acquire this new direct dependency. With vcpkg,
select the manifest's `signing` feature as well (`VCPKG_MANIFEST_FEATURES=signing`).
Installed packages expose `A2A_CPP_HAS_AGENT_CARD_SIGNING` and the signing target
when enabled. Canonicalization remains available in `a2a::core` through
`a2a/core/json_canonicalization.h`.

```cpp
auto signed_card = a2a::core::SignAgentCard(card, key_id, private_key_pem);
auto verified = a2a::core::VerifyAgentCardJson(raw_json, key_resolver, now);
```

The canonicalizer validates JSON, rejects duplicate keys, invalid Unicode and
non-finite numbers, sorts object members by UTF-16 code units, and formats
IEEE-754 numbers using ECMAScript/JCS decimal thresholds. Arrays retain order.
Input is limited to 1 MiB and 128 nesting levels. `CanonicalizeJson` is public
for independent RFC 8785 verification. Parsing uses the existing ProtoJSON
helpers with a generic protobuf `Value`, retaining unknown fields. A shared
strict syntax check rejects parser extensions such as trailing commas and
non-JSON escapes. No additional JSON library is required.

Verification operates on the **original discovery JSON**, including unknown
fields. At least one supplied signature must verify using EdDSA and a trusted
key accepted by the application's resolver. Unknown, invalid, expired and
revoked signatures do not invalidate another trusted signature, allowing key
rotation. If no signature verifies, the card is rejected. The resolver returns
a public key, validity interval and revocation status. The caller supplies time
for deterministic policies and testing.

`typ`, `jku` and other noncritical protected parameters are accepted. Unprotected
annotations are allowed, but overlapping protected/unprotected parameter names
are rejected. This Ed25519 profile implements no critical JWS extensions and
rejects `crit` and unencoded payloads. It does not support ES256 or RS256 yet.
`jku` never triggers a network fetch; applications provision trusted key IDs.
Verification preserves received JSON members apart from `signatures`; signing
uses the SDK's protobuf field-presence serialization. The shared SDK fixture
covers a payload both SDKs canonicalize identically. Cross-SDK treatment of
empty/default fields remains an upstream clarification
([A2A #2122](https://github.com/a2aproject/A2A/issues/2122)); this profile does not
silently remove arbitrary extension fields or empty values during verification.

Configure discovery verification before fetching cards:

```cpp
discovery.SetAgentCardVerifier([&](std::string_view json) {
  return a2a::core::VerifyAgentCardJson(json, key_resolver, current_time());
});
```

Discovery verifies before protobuf parsing and caching. Cached JSON is verified
again on each cache hit, so key revocation and expiration take effect without
waiting for cache expiry. A failed cached verification discards the entry and
fetches a replacement, which must pass the current verifier. A failed refresh
never returns the invalid cached card. Extended discovery uses the same verifier. Discovery
without an explicitly configured verifier retains its existing unsigned-card
behavior. Applications decide whether signed cards are required and how trusted
key IDs are provisioned; keys are never fetched from an untrusted card header.

The SUT signs its public and extended cards when
`A2A_TCK_SIGNING_KEY_FILE` and `A2A_TCK_SIGNING_KEY_ID` are configured. It uses the
SDK signing API and re-signs the extended card after changing its contents.
The [live scenarios](security-scenarios.md) consume the card using SDK discovery
and verification. `JsonCanonicalizationTest` checks the language-neutral RFC 8785 corpus published
by the Python SDK (with its provenance retained). `AgentCardSigningTest` checks
the Python/Go shared Ed25519 vector and independently signed JWS header cases,
plus
signature exclusion, tampering, invalid signing keys, key validity and cached
key-policy changes.

Requirement mapping: `CARD-SIGN-001..004`.
