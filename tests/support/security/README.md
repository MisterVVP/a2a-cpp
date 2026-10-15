# Security vectors

`jcs_vectors.json` is the language-neutral `a2a-jcs-v01` corpus copied from
`a2aproject/a2a-python` at `0ee47d7baa47a706e33934a4e4c3d46bad16298f`.
Its original provenance and independent RFC 8785 oracles are recorded in the
file. Two rejection cases concern Agent Card signature-field exclusion rather
than generic JSON syntax; the canonicalization test checks them separately.

`sdk_signing_vector.json` contains the Ed25519 golden card published by
`a2aproject/a2a-go` at `e85760e87ce007bfbd02e38374a4ce2fc2037fe3` in
`a2acrypto/testdata/golden.json`. That SDK's `gen_golden.py` generates the vector
using the Python SDK, and the Go SDK tests reproduce the same signature. The
public key is derived from its published test seed. Both SDK repositories use
the Apache-2.0 license, as does this project.

Its `vectors` array contains additional independently signed header cases
using the same published test key and OpenSSL's command-line Ed25519 signer.
The ASCII-only payload uses Python's sorted, compact JSON representation;
these payload bytes match the SDK golden card. The cases check permitted
`jku`/noncritical headers and rejection of unsupported critical extensions and
unencoded payloads. These are public test keys, never deployment credentials.
