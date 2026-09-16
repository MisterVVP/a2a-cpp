# Releases and Versions

This documentation is intended to stay version-aware without embedding a release number in every page title.

## Current documented release

The current documented release is **v0.5.0** and is recommended for new consumers.

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

## Previous release: `v0.4.1`

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

- Pin CMake `FetchContent` integrations to a release tag such as `v0.5.0` or to
  a reviewed commit.
- Prefer `find_package(a2a_cpp CONFIG REQUIRED)` for installed SDK packages.
- Keep generated protobuf headers and linked SDK libraries from the same
  installed package or build tree.
- Review release notes before upgrading between versions.

## Documentation policy

Page titles and navigation should remain mostly version agnostic. Release-specific notes belong on this page or in clearly marked compatibility sections.
