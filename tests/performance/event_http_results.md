# Event HTTP runtime validation snapshot

Captured on 2026-11-08 with GCC 14.2.0, `RelWithDebInfo`, Boost 1.92.0,
simdjson 4.6.2, protobuf 3.21.12 and gRPC 1.51.1. Both builds used the same
compiler, dependencies and enabled features. The baseline was an unmodified
`release/v0.6.0` checkout at `40e70a239d18208e07738abb64415d9d97e1a601`;
the event binary included this issue's working-tree implementation.

The Linux container had a two-core CPU quota and 8 GiB memory limit. The Python
client shared that quota with the server. Each case started a fresh process;
4,096 requests used persistent connections and the same fixture backend,
in-memory store, request data and protocol headers. A connection barrier ensured
all concurrent clients connected before sending. The native fixture bridges the
existing synchronous application through the SDK's bounded executor adapter.
The unary workload is agent-card discovery; the finite SSE workload is
`POST /a2a/message:stream`. See [runner instructions](README.md#event-http-runtime-comparison)
and [raw measurements](event_http_results.json). These short local measurements
are reproducible evidence, not production capacity or cross-platform claims.

## Latency and throughput

All active cases reported **zero protocol/network errors**. Latencies are in
milliseconds. First-event values apply only to SSE. Throughput includes client
setup and completion of the full batch; percentiles measure individual requests.

| Runtime | Workload | Clients | Requests/s | Completion p95 / p99 | First event p95 / p99 |
| --- | --- | ---: | ---: | ---: | ---: |
| Sync | agent_card | 1 | 6079 | 0.269 / 0.315 | — |
| Sync | finite_sse | 1 | 7963 | 0.158 / 0.236 | 0.140 / 0.213 |
| Sync | agent_card | 16 | 7047 | 5.342 / 8.026 | — |
| Sync | finite_sse | 16 | 4948 | 6.840 / 9.451 | 6.795 / 9.431 |
| Sync | agent_card | 64 | 7139 | 20.652 / 30.026 | — |
| Sync | finite_sse | 64 | 4621 | 30.108 / 41.722 | 30.081 / 41.702 |
| Event | agent_card | 1 | 5420 | 0.244 / 0.397 | — |
| Event | finite_sse | 1 | 2986 | 0.437 / 0.581 | 0.322 / 0.462 |
| Event | agent_card | 16 | 6822 | 5.454 / 14.351 | — |
| Event | finite_sse | 16 | 3589 | 9.870 / 17.187 | 9.292 / 16.317 |
| Event | agent_card | 64 | 6831 | 22.367 / 32.644 | — |
| Event | finite_sse | 64 | 3913 | 36.452 / 50.151 | 36.014 / 49.319 |

## Process resources

CPU is process CPU time divided by elapsed time, where 100% means one core.
RSS and threads are maxima sampled every 20 ms. The separate idle case held
64 sockets open for one second without application requests. These figures
include the SUT's gRPC and application infrastructure as well as HTTP networking.

| Runtime | Workload | Clients | CPU % | Peak RSS KiB | Peak threads |
| --- | --- | ---: | ---: | ---: | ---: |
| Sync | agent_card | 1 | 60.8 | 16752 | 10 |
| Sync | finite_sse | 1 | 54.4 | 19568 | 10 |
| Sync | agent_card | 16 | 89.5 | 17888 | 25 |
| Sync | finite_sse | 16 | 55.6 | 20600 | 25 |
| Sync | agent_card | 64 | 94.1 | 20796 | 73 |
| Sync | finite_sse | 64 | 57.5 | 22896 | 73 |
| Sync | idle | 64 | 1.0 | 17072 | 73 |
| Event | agent_card | 1 | 74.1 | 17544 | 21 |
| Event | finite_sse | 1 | 87.5 | 20204 | 21 |
| Event | agent_card | 16 | 99.9 | 17760 | 21 |
| Event | finite_sse | 16 | 106.9 | 20756 | 21 |
| Event | agent_card | 64 | 101.7 | 18528 | 21 |
| Event | finite_sse | 64 | 103.2 | 22660 | 21 |
| Event | idle | 64 | 1.0 | 16716 | 21 |

The event fixture kept **21 threads** at every concurrency and for 64 idle
connections; the synchronous fixture grew from 10 to 73. At 64 idle connections,
RSS was 16,716 versus 17,072 KiB, with approximately 1% sampled CPU in both.
At c64, event discovery throughput was 4% lower and finite SSE throughput was
15% lower than the baseline. The shared Python client and two-core quota limit
how much can be inferred about server saturation from these numbers.

The low-concurrency cost is material and intentional: c1 finite SSE completed
2,986 versus 7,963 requests/s, with completion p99 0.581 versus 0.236 ms and
first-event p99 0.462 versus 0.213 ms. The synchronous fixture executes the finite
producer and socket writes directly on its connection thread. The native fixture
adds bounded legacy-backend dispatch, HTTP/native executor hand-offs for header
selection, and an acknowledgement after each asynchronous socket write. Subsequent
events use the dispatcher strand's socket sink directly to avoid a mailbox hop
per event. Awaited writes and cancellation preserve bounded slow-reader behavior;
they add scheduling cost to very short local streams. The API remains opt-in,
and the synchronous path remains useful for that workload. The c1 discovery
throughput cost was 11%; it crosses the fixed application pool instead of running
inline on a connection thread. This snapshot does not claim throughput parity.

## Validation

- `./scripts/verify_changes.sh`: exit 0; 1,091 registered CTest cases, zero failures,
  three existing skips, including installed-consumer coverage.
- Exact CI clang-format dry run and `./scripts/run_clang_tidy.sh build`: exit 0.
  Local lint uses clang-tidy 19; unchanged translation units reuse input-validated
  results from this session's full source and test passes.
- Default SDK without Asio or curl: clean configure/build passed.
- Native lifecycle stress: 100 iterations of the live-stream, disconnect, and
  delayed-backend-cleanup cases passed, including 32 live streams on one native
  worker and one HTTP application worker.
- Mandatory performance smoke: 54 rows, zero errors, HTTP+JSON and gRPC.
- `mdbook build book`: passed. Linux native socket tests ran locally; macOS,
  Windows and sanitizer runs are configured in CI and require CI execution.
- Dependency manifests and pins are unchanged. Static analysis and security tests
  passed; an external OSV query was blocked by the environment proxy (CONNECT 403),
  so this snapshot does not assert a successful online vulnerability scan.

The unmodified upstream TCK at
`104a7b4ae0d83f65c075e3793e322953a830eaba` ran through the repository scripts
against gRPC, JSON-RPC and HTTP+JSON. Both the clean release and event runtime
reported **242 passed, 17 skipped, four failed**, with identical per-requirement
statuses:

- `TestRestErrorStructure::test_error_uses_aip193_format` requires
  `application/json`; the existing REST error serializer returns
  `application/a2a+json`.
- `test_must_requirement[CORE-SEND-003-{grpc,jsonrpc,http_json}]` rejects the
  SDK's unsupported-content error. The upstream requirement describes that error
  but omits `expected_error`, so the generic validator expects success.

No upstream TCK files or expectations were changed. This is not 100% mandatory
conformance. The maintainer explicitly authorized publishing this implementation with the
existing TCK failures and blocked online vulnerability scan documented as
validation exceptions. The failures remain unresolved.
