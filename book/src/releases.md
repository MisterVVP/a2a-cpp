# Releases and Versions

This documentation is intended to stay version-aware without embedding a release number in every page title.

## Current documented release

The current documented release is **v0.4.0** and is recommended for new consumers.

### Highlights since `v0.3.0`

- Configurable PostgreSQL connection pool capacity through
  `PostgresStoreOptions::connection_pool_size`, with the backward-compatible
  default of `4`.
- Shared configured PostgreSQL pools for task and push-notification stores
  created through `CreateStoreBundle()`.
- PostgreSQL operation-phase diagnostics, pool-size metadata, median
  aggregates, and query-plan evidence in performance reports.
- Removal of executor-wide request serialization that limited independent
  concurrent operations.
- Bounded protocol-facing `ListTasks` pagination across HTTP+JSON, JSON-RPC,
  and gRPC, with a default page size of `50` and a maximum of `100`.
- Optimized in-memory task listing, pagination, filtering, artifact exclusion,
  and history projection, backed by expanded benchmark thresholds.
- Interruptible gRPC stream cancellation watching, eliminating the previous
  approximately 50 ms normal stream-completion floor.
- Deterministic follow-up and decomposed push-notification performance
  scenarios with clearer workload attribution.
- Expanded normal and PostgreSQL-tail performance matrices, reports, tests,
  and parallel CI execution.
- Centralized HTTP server response construction and reusable server-side
  `A2A-Version` header validation.
- Corrected benchmark-runner module paths and refreshed README, installation,
  storage, pagination, and performance documentation.

### Compatibility notes

There are no intentional public API removals in `v0.4.0`.

Protocol-facing `ListTasks` requests are now bounded consistently across
transports. An omitted page size returns at most `50` tasks, explicit values
must be between `1` and `100`, and callers must follow `next_page_token` to
retrieve additional pages. Internal store calls may still use the documented
unbounded sentinel where appropriate.

The PostgreSQL connection pool still defaults to `4`, so existing applications
retain their previous capacity unless they explicitly configure a different
value. Applications should size the pool for expected concurrent database
operations while respecting PostgreSQL connection limits.

Built-in HTTP+JSON and JSON-RPC SSE clients continue to require libcurl support.
Custom HTTP requester and streaming requester implementations remain supported
for consumers that need alternative transport stacks.

## Previous release: `v0.3.0`

`v0.3.0` introduced production-ready SSE streaming for HTTP+JSON and JSON-RPC,
end-to-end streaming and subscriptions across all supported transports,
report-only performance testing, repository-local vcpkg overlay packaging, and
expanded build and platform documentation.

Its TCK validation snapshot was:

- 249 tests passed
- 16 tests skipped
- 0 executed test failures
- 100.0% compatibility for tested, non-skipped requirements

## Versioning guidance

- Pin CMake `FetchContent` integrations to a release tag such as `v0.4.0` or to
  a reviewed commit.
- Prefer `find_package(a2a_cpp CONFIG REQUIRED)` for installed SDK packages.
- Keep generated protobuf headers and linked SDK libraries from the same
  installed package or build tree.
- Review release notes before upgrading between minor versions.

## Documentation policy

Page titles and navigation should remain mostly version agnostic. Release-specific notes belong on this page or in clearly marked compatibility sections.
