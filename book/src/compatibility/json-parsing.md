# JSON parsing

The SDK uses simdjson 3.0+ for JSON syntax, UTF-8 validation, SDK message decoding,
and transport envelopes. This is a required core dependency even with
`A2A_ENABLE_SIGNING=OFF`. OpenSSL remains optional. No public SDK signature exposes
simdjson types. Protobuf continues to serialize messages.

## Parsing inventory

| Entry point | Implementation and remaining conversions |
| --- | --- |
| `core::JsonToMessage` | Shared simdjson validation followed by reflection-based decoding of ordinary SDK fields. Both protobuf and JSON field names work. Unknown-field and strict-null/duplicate options are preserved. |
| Agent Card discovery (including discovery for gRPC) | Shared decoder; the verifier still receives the original, complete JSON before typed decoding, preserving unknown signed fields. Native gRPC messages use protobuf wire decoding. |
| HTTP+JSON requests, responses and errors | Existing binding validation and error tagging call the shared decoder; `google.rpc.Status` and metadata remain protobuf messages. |
| JSON-RPC client unary results, errors and SSE | Shared envelope parser; the original result JSON is decoded directly. Removed intermediate protobuf `Struct`/`Value`, result replacement, and separate ListTasks/SSE scanners. |
| JSON-RPC server requests and push configurations | Shared envelope parser; original `params` JSON reaches the shared decoder. Removed the transport's flat-message reflection parser. IDs retain their existing protobuf `Value` representation for outgoing envelope serialization. |
| JSON-RPC ListTasks binding parameters | Shared simdjson-to-`Struct` conversion followed by the existing binding-specific filters and validation. This `Struct` remains to preserve binding errors and legacy parameter semantics. |
| HTTP+JSON and JSON-RPC SSE event bodies | Shared message decoder. SSE framing remains in `SseParser`, which parses event framing rather than JSON. |
| Push-notification configuration payloads | Shared decoder through each binding; delivery serializes task payloads and does not parse callback bodies. |
| `Struct`, `Value`, `ListValue` | simdjson-to-protobuf conversion; these objects are the actual application data model, rather than an intermediate representation of a typed message. Numbers retain protobuf `Value`'s binary64 semantics. |
| Bytes, uncommon numeric scalar types, non-string map keys and well-known messages | Protobuf's semantic decoder on original JSON tokens. This deliberately retains base64 variants, exact int64/range rules, timestamps, durations, wrappers, `Any` and FieldMask behavior without reimplementing those codecs. Bytes use the protobuf wrapper decoder only for that field; uncommon numeric types and non-string maps use the fallback for their message subtree. Quoted/exponent int32 tokens use the protobuf wrapper decoder. Null-bearing typed messages retain protobuf decoding because supported protobuf versions differ on null container entries; strict SDK validation runs first. |
| Outgoing JSON-RPC serialization | Existing protobuf `Value` bridges remain where envelope writers require them; parsing these generated payloads uses the shared implementation. Serialization redesign is outside this migration. |
| Signed Agent Cards | The existing signing parser and dedicated RFC 8785 serializer remain. simdjson does not implement JCS; general parsing never reconstructs the input used for signature verification. |

The former public structural range helper `a2a/core/json_value.h` was removed with
its custom grammar, tests and scanner-only benchmarks. SDK transports no longer
need it.

## Ownership and input policy

Each internal document exclusively leases a DOM parser from a thread-local cache.
Nested/reentrant parses acquire separate parsers; there is no process-wide lock.
The on-demand cursor owns padded input and its own parser. DOM storage and cursor
views never escape into SDK results or application callbacks. Returned protobuf
messages and envelope JSON strings own their data.

The complete input is validated before decoding or skipping unknown fields. A
second on-demand pass reads original tokens for typed payloads and subtree
fallbacks, avoiding binary64 rounding of decimal integers during extraction.
JSON-RPC envelopes must be objects and reject duplicate decoded member names,
including escaped spellings. Typed strict parsing also rejects duplicate known
fields/aliases in nested messages. Protobuf data maps retain their normal map
semantics. Binding-specific null restrictions remain in place; ordinary protobuf
nulls leave fields unset.

Inputs are limited to 64 MiB and nesting depth 128. Malformed JSON, trailing data,
invalid UTF-8 and numbers outside simdjson's supported numeric range return
`Error::Serialization`; transports retain their HTTP status, transport tag and
binding-specific error mapping. Protobuf numeric fallback receives original tokens,
so rounding cannot make an invalid integer acceptable. Signing retains its own
stricter JCS number policy and existing limits.

## Validation and measurements

The shared golden/parity fixtures exercise aliases, enums, int32/exponent limits,
int64 precision, bytes, timestamps, metadata, nulls and oneofs. Existing transport,
streaming, discovery and signing suites exercise the migrated entry points.
Cross-binding scenarios include binary parts and null/list metadata.

The parsing benchmarks include small messages, Agent Cards, task lists, SSE event
messages, and a task with 256 additional history messages and a 16 KiB binary artifact. Client and
server benchmarks measure binding processing separately from parser-only timing.
One Linux sample compared `bed242d` with this migration using GCC 15.3,
protobuf 3.21.12 and simdjson 3.12.3, Release builds with signing disabled, and
identical Google Benchmark definitions. Values are median CPU microseconds from
three repetitions with a 0.1-second minimum per repetition on a shared host.
These are indicative measurements, rather than cross-platform guarantees.

| Workload | Before (µs) | After (µs) |
| --- | ---: | ---: |
| Small SendMessage request | 12.78 | 1.43 |
| Agent Card | 21.14 | 2.00 |
| SSE StreamResponse payload | 21.20 | 3.84 |
| Large task history/artifact (290,486 bytes) | 2,035.36 | 339.05 |
| HTTP+JSON client ListTasks, 200 tasks | 638.64 | 629.12 |
| JSON-RPC client ListTasks, 200 tasks | 876.59 | 621.81 |
| JSON-RPC server GetTask, in-process binding/dispatch | 110.22 | 42.79 |

A separate warmed allocation probe counted C++ `new`/`new[]` calls: small message
305 → 10, Agent Card 487 → 25, SSE payload 479 → 60, large task 5,161 → 1,668,
and mocked JSON-RPC GetTask 1,313 → 233. This excludes direct `malloc` calls and
retained parser buffers. Whole-process peak RSS was approximately 13 MiB for both
probe executables, so these results do not establish lower resident memory.

The required network performance smoke check produced 54 result rows with zero
errors. It is a correctness smoke check; parser-only and in-process timings do
not establish network latency or throughput improvements. Full native validation
passed 804 configured CTest checks with two expected TLS skips. Unmodified
upstream TCK runs across all three transports and both stores each passed 249
tests with 16 conditional skips and no failed assertions. Upstream reports
80.9% mandatory / 80.0% overall compliance because 25 registry requirements have
no executable fixtures; these gaps remain explicit in the PR validation report.
