# Releases and Versions

This documentation is intended to stay version-aware without embedding a release number in every page title.

## Unreleased development toward `v0.6.0`

`ListTasks` development replaces numeric offsets with opaque, versioned cursors
and orders tasks by storage update time descending, then task ID descending.
Clients must restart old pagination sequences with an empty token. Custom stores
used with secured listing must supply aligned cursor metadata, and externally
managed PostgreSQL schemas need the ordering-index migration. See the
[ListTasks cursor contract](server/list-tasks-pagination.md) for compatibility,
consistency and migration details.

SDK JSON parsing now requires simdjson 3.0+ even when signing is disabled.
The obsolete public `a2a/core/json_value.h` structural scanner was removed.
General JSON input is limited to 64 MiB and nesting depth 128. See the
[JSON parsing inventory](compatibility/json-parsing.md) for migration paths,
protobuf compatibility exceptions, and ownership/input policies.

The stable `v0.5.2` behavior and release notes below remain unchanged.

## Current documented release

The current documented release is **v0.5.2** and is recommended for new consumers.

### Highlights since `v0.5.1`

#### SDK security and signed Agent Cards

The dispatcher can authenticate callers, enforce operation and task permissions,
filter visible task counts and pages, and recheck permissions around blocking
stream reads. Hidden and missing tasks share the same errors. The new
`TaskAuthorizationService` persists explanatory `auth_required` states and
coordinates revision-checked transitions with live subscriptions. Applications
supply identity validation, permission rules and credential policy.

HTTP clients support certificate and hostname verification with per-call TLS
options; gRPC uses native credentials. Optional Ed25519/JWS Agent Card signing
uses RFC 8785 canonicalization and verifies original JSON before conversion to
SDK types. Discovery preserves unknown signed fields and can revalidate cached
cards against key rotation, revocation and expiry policies. Signing is disabled
by default; enabling `A2A_ENABLE_SIGNING` and linking `a2a::signing` requires
OpenSSL 3.0+ and simdjson 3.0+. See the
[security scenarios guide](auth/security.md) for
application policy, supported signing features and live-test coverage.

#### Protocol versions and binding validation

Public C++ clients send A2A protocol versions on every operation across
HTTP+JSON, JSON-RPC and gRPC. Negotiation matches Major.Minor and ignores numeric
patch components. Servers reject malformed and unsupported versions before
dispatch. Empty or omitted versions select the unsupported A2A 0.3 fallback and
are rejected; clients should explicitly send `1.0`. `CallOptions::protocol_version`
allows an explicit selection. SDK release `0.5.2` and A2A protocol `1.0` are
separate version numbers.

Independent C++ integration scenarios exercise all nine ordered binding pairs
against a shared agent with both in-memory and PostgreSQL stores. SDK evidence
and unmodified upstream TCK coverage are reported separately; passing SDK tests
does not increase upstream coverage percentages.

#### Benchmark trends and contributor validation

CI retains compact benchmark trend artifacts and compares median timings with
the preceding successful main run. A configurable noise band and advisory
signals help identify changes that warrant confirmation across multiple runs;
existing benchmark thresholds are unchanged. Go-only benchmark-runner changes
have a focused formatting, vet, race-test and build validation path.

#### Documentation consolidation

Public SDK guidance now lives in this book, including complete
[storage and schema migration](server/storage.md), [security integration](auth/security.md),
[examples and tutorials](examples/overview.md), and [performance testing](performance/workloads.md).
Source-folder READMEs link to the SDK guides. Repository-wide contribution,
agent, coding, security-reporting and support policies remain authoritative in
the root repository files; they are not duplicated in this book.
Duplicate guides and historical internal planning records are removed from the
public documentation tree.

### Compatibility notes

This release retains numeric `ListTasks` continuation tokens and creation-order
listing. Omitted protocol-facing page sizes still default to `50`, and explicit
sizes must be between `1` and `100`. Clients should copy `next_page_token`
unchanged. Opaque cursors and descending update-time ordering from
[PR #312](https://github.com/MisterVVP/a2a-cpp/pull/312) are reserved for `v0.6.0`.
The broader SDK JSON parsing migration in
[issue #311](https://github.com/MisterVVP/a2a-cpp/issues/311) is follow-up work;
this release uses simdjson only in the optional signing path.

Raw clients that omit the A2A version must send `A2A-Version: 1.0` (or gRPC
`a2a-version` metadata). Configured security policies now enforce access before
returning resources and can reject calls previously accepted without those
checks. Review the application callbacks and task-authorization contract before
upgrading secured deployments. Existing applications can continue with signing
disabled; ordinary builds do not require OpenSSL or simdjson for signing.

Rebuild the SDK and downstream binaries together. Public request/context and
dispatcher layouts have changed since `v0.5.1`; this release does not promise
binary compatibility. Existing PostgreSQL schema and privilege requirements
remain in effect; this release does not include the cursor-ordering indexes.

## Previous release: `v0.5.1`

The following notes describe `v0.5.1`.

### Highlights since `v0.5.0`

#### Multi-agent tutorials and MCP resources

Two standalone C++20 tutorials demonstrate Agent Card discovery, A2A delegation
from a coordinator to a specialist, and structured artifacts: Job Application
Assistant and Customer Support Copilot. They default to deterministic model responses and
also support an optional OpenAI-compatible model endpoint.

The tutorial-local MCP client supports initialization, session handling, and
textual `resources/read` validation. Job Application Assistant accepts local
inputs or MCP resources; Customer Support Copilot retrieves tickets through
OAuth-protected ContextForge with Keycloak. Docker Compose smoke flows cover
both tutorials, while the native runner covers Job Application Assistant. See
the [production tutorials guide](examples/tutorials.md)
for setup and model configuration. This MCP integration is tutorial-local, not
a new general-purpose public SDK client.

#### Streaming and subscription reliability

Synchronous HTTP stream completion now retains shared completion state through
callback notification, avoiding lifetime races. Subscription cancellation waits
until coroutine resumers have released the frame-owned resume mutex before
destroying the frame. gRPC transports use timed reads for sessions that support
them, observing cancellation without invoking `Cancel()` concurrently with a
blocking `Next()`; other sessions retain the cancellation-watcher fallback.

A focused [ThreadSanitizer profile](contributing/thread-sanitizer.md)
and CI job exercise SDK-owned streaming, subscription, reactor, and in-memory
store concurrency without a suppression file.

#### Performance harness reliability

Wire benchmarks now use a dedicated `performance_sut` entrypoint, keeping
performance diagnostic endpoints and counters out of the conformance-focused
`tck_sut`. Both share the transport lifecycle runtime. Each wire scenario runs
as a separate driver invocation against the shared SUT; periodic health checks
report unexpected SUT exits with the active workload, exit code or POSIX signal,
and log tail, then stop the driver promptly.

This release improves diagnostic isolation and failure reporting. It does not
introduce a separate non-blocking performance HTTP server or claim a new
measured throughput improvement.

#### Cross-platform test execution and portability

Windows and macOS CI now explicitly enable testing and run CTest after the
build, with failure output and an error for an empty suite. macOS selects all
registered tests; Windows selects all C++ tests and applicable Python checks,
excluding only three documented script tests that depend on POSIX execution or
a compilation database unavailable with Visual Studio.

The expanded execution caught and fixed libcurl test selection, a client-test
lifetime error, gRPC unit-test runtime initialization, accepted HTTP socket mode
and idle shutdown differences, and protobuf-version-dependent duplicate field
alias handling in flat JSON-RPC requests. HTTP persistence regressions cover
sequential reuse, pipelining, fragmented requests, concurrency, and idle shutdown.

### Compatibility notes

No public APIs are intentionally removed. `ServerStreamSession` gains a virtual
`SupportsTimedNext() const noexcept` method whose default is `false`, so existing
derived classes remain source-compatible. Sessions opting in must implement the
timed-read contract through `NextFor()`. The public HTTP header lookup helper
also gains an overload for ordered header containers. Rebuild the SDK and
downstream binaries together; the added virtual method changes the class vtable
and this release does not promise binary compatibility with `v0.5.0`.

Custom performance tooling should use `performance_sut` and `A2A_PERF_SUT`;
conformance tooling continues to use `tck_sut`. The historical wire result value
`driver_type=wire_tck_sut` remains unchanged for report compatibility. Pagination
and PostgreSQL schema/privilege requirements from `v0.5.0` remain in effect.

## Earlier release: `v0.5.0`

The following notes describe `v0.5.0`; its benchmark comparisons are historical
evidence for that release, not measurements of `v0.5.1`.

### Highlights since `v0.4.1`

#### PostgreSQL write scalability

The built-in PostgreSQL stores reduce redundant task reads and database round
trips on write paths. Push-configuration create and delete coordination now uses
schema- and task-scoped transaction advisory locks instead of task-row
`FOR KEY SHARE` locks, and the push-configuration schema reduces index and write
amplification. Task-history persistence uses a bounded, optimistic,
revision-aware update, with `created_sequence` included in compare-and-swap
protection against delete/recreate ABA cases. These changes preserve transaction,
task-history, concurrency, and least-privilege correctness.

In normal performance artifacts collected on GitHub-hosted runners, the
geometric-mean movement across the c1, c4, c16, and c64 PostgreSQL in-process
coordinates was approximately:

| Scenario | Throughput | p95 |
|---|---:|---:|
| `SendMessage_CreateTask` | +60.3% | -36.4% |
| `SendMessage_FollowUpExistingTask` | +51.5% | -33.9% |
| `PushConfig_Create` | +20.6% | -1.7% |
| `PushConfig_CreateMany` | +22.3% | -19.8% |

These controlled CI measurements are benchmark evidence, not guaranteed
application performance. In particular, the `PushConfig_Create` c64 p95
coordinate was noisy across independent runs; the results do not imply that
all PostgreSQL tail-latency coordinates improved.

#### Streaming scalability and latency

Synchronous HTTP streaming now runs through bounded, process-wide libcurl
multi-reactors rather than assigning an OS worker thread to every stream by
default. Linux builds integrate `epoll`, `eventfd`, and `timerfd`; other
platforms use a portable libcurl multi-event fallback. A separate bounded
callback executor preserves serialized callback ordering within each stream and
limits callback backlog without blocking network progress.

The streaming paths also reduce SSE parsing and framing allocations and copies,
avoid unnecessary JSON/protobuf conversions, and safely reuse HTTP connections
after finite streams. Cancellation, shutdown, object destruction, and callback
races are handled without changing the existing public synchronous APIs.

A c64 in-memory wire comparison between the v0.4.1 normal performance artifact
`9277084128` and current-main artifact `10403618734` reported:

| Scenario | Transport | Throughput delta | First-event p95 delta | Completion p95 delta |
|---|---|---:|---:|---:|
| Finite stream | gRPC | +21.3% | -13.6% | -15.2% |
| Finite stream | HTTP+JSON | +86.6% | -37.6% | -36.6% |
| Finite stream | JSON-RPC | +92.1% | -38.9% | -38.8% |
| Subscribe | gRPC | +12.4% | -21.4% | -13.6% |
| Subscribe | HTTP+JSON | +32.7% | -52.6% | -43.3% |
| Subscribe | JSON-RPC | +21.6% | -48.0% | -40.2% |

Both artifacts reported zero operation errors. As with the PostgreSQL results,
these controlled CI measurements characterize those runs and are not universal
production guarantees.

#### Performance observability

Performance tooling now provides focused PostgreSQL write diagnostics, including
wait-event, lock, WAL, and buffer attribution, plus command-level phase
diagnostics. Markdown reports expose first-event and stream-completion p50/p95
columns, streaming-specific rollups, and `n/a` rather than misleading zeroes for
streaming dimensions that do not apply.

#### PostgreSQL production hardening

Managed task-aware push schemas now validate during startup that the effective
push-store role can execute
`a2a_lock_task_for_push_config(TEXT)`. Administrative `TRUNCATE a2a_tasks`
operations remove locally owned push configurations while preserving externally
owned configurations.

SDK-managed schemas migrate to the current task-aware push-configuration schema
version. Operators of externally managed PostgreSQL schemas must apply the
helper, trigger, and privilege requirements in the storage documentation;
missing required schema objects or privileges fail validation early.

### Compatibility notes

There are no intentional public API removals in `v0.5.0`. Existing synchronous
`A2AClient`, streaming observer, transport, and server APIs remain
source-compatible. HTTP streaming execution changed internally, not at the
public API level.

The pagination behavior documented for `v0.4.1` remains unchanged: omitted
protocol-facing `ListTasks.page_size` defaults to `50`, explicit values must be
between `1` and `100`, and callers follow `next_page_token` for additional
pages.

PostgreSQL users should allow SDK-managed schemas to migrate. Externally managed
schemas may require migration to satisfy the current validation contract,
including helper functions, trigger wiring, schema access, and function
`EXECUTE` privileges. The task-aware push-configuration delete and truncate
coordination must run at `READ COMMITTED`; repeatable-read and serializable
transactions are rejected because a stale snapshot cannot safely perform the
required cleanup. These are operational schema requirements rather than public
C++ API removals.

## Earlier release: `v0.4.1`

`v0.4.1` optimized PostgreSQL push-configuration and task persistence paths,
added HTTP/1.1 reuse for unary HTTP transports, hardened typed `ListTasks`
validation, expanded component performance diagnostics, and introduced
repository-wide Conventional Commit validation.

Its compatibility behavior remains relevant: public include paths and exported
CMake targets were unchanged by the internal transport source reorganization,
and valid `ListTasks` responses remain compatible across HTTP+JSON and JSON-RPC.

## Earlier release: `v0.4.0`

`v0.4.0` focused on configurable PostgreSQL pool sizing, bounded protocol-facing
`ListTasks` pagination, optimized in-memory task listing, interruptible gRPC
stream cancellation, expanded performance validation, and centralized HTTP
server response construction.

## Versioning guidance

- Pin CMake `FetchContent` integrations to a release tag such as `v0.5.2` or to
  a reviewed commit.
- Prefer `find_package(a2a_cpp CONFIG REQUIRED)` for installed SDK packages.
- Keep generated protobuf headers and linked SDK libraries from the same
  installed package or build tree.
- Review release notes before upgrading between versions.

## Documentation policy

Page titles and navigation should remain mostly version agnostic. Release-specific notes belong on this page or in clearly marked compatibility sections.
