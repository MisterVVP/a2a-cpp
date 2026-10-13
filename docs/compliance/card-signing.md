# Signed Agent Cards

`SignAgentCard` signs public protobuf Agent Cards using an Ed25519 PEM private key
and a nonempty key ID. Cryptographic operations use OpenSSL. The protected JWS
header contains `alg: EdDSA` and `kid`; the payload excludes the entire
`signatures` field and uses RFC 8785 JSON Canonicalization Scheme (JCS).

```cpp
auto signed_card = a2a::core::SignAgentCard(card, key_id, private_key_pem);
auto verified = a2a::core::VerifyAgentCardJson(raw_json, key_resolver, now);
```

The canonicalizer validates JSON, rejects duplicate keys, invalid Unicode and
non-finite numbers, sorts object members by UTF-16 code units, and formats
IEEE-754 numbers using ECMAScript/JCS decimal thresholds. Arrays retain order.
Input is limited to 1 MiB and 128 nesting levels. `CanonicalizeJson` is public
for independent RFC 8785 verification. Parsing uses nlohmann JSON; signing and
verification use OpenSSL's Ed25519 implementation.

Verification operates on the **original discovery JSON**, including unknown
fields. Every supplied signature must be valid, use EdDSA and identify a key
accepted by the application's resolver. The resolver returns a public key,
validity interval and revocation status. Verification rejects expired, not-yet-
valid, revoked and unknown keys. The caller supplies time for deterministic
policies and testing. Additional protected/unprotected header parameters are
currently rejected; this API supports the SDK's `alg`/`kid` EdDSA profile.

Configure discovery verification before fetching cards:

```cpp
discovery.SetAgentCardVerifier([&](std::string_view json) {
  return a2a::core::VerifyAgentCardJson(json, key_resolver, current_time());
});
```

Discovery verifies before protobuf parsing and caching. Cached JSON is verified
again on each cache hit, so key revocation and expiration take effect without
waiting for cache expiry. Extended discovery uses the same verifier. Discovery
without an explicitly configured verifier retains its existing unsigned-card
behavior. Applications decide whether signed cards are required and how trusted
key IDs are provisioned; keys are never fetched from an untrusted card header.

The SUT signs its public and extended cards when
`A2A_TCK_SIGNING_KEY_FILE` and `A2A_TCK_SIGNING_KEY_ID` are configured. It uses the
SDK signing API and re-signs the extended card after changing its contents.
The [live scenarios](security-scenarios.md) consume the card using SDK discovery
and verification. `AgentCardSigningTest` separately checks RFC vectors,
signature exclusion, tampering, invalid signing keys, key validity and cached
key-policy changes.

Requirement mapping: `CARD-SIGN-001..004`.
