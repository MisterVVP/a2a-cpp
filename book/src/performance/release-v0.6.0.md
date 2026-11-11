# Integrated SDK v0.6.0 release validation

This record covers [#328](https://github.com/MisterVVP/a2a-cpp/issues/328) and
[#329](https://github.com/MisterVVP/a2a-cpp/issues/329). SDK **v0.6.0** targets
**A2A protocol 1.0 (v1.0.1 specification)**. Local evidence is separate from
release approval and full upstream TCK conformance.

## Tested source and host

The baseline is SDK v0.5.2 commit
`b58ebd8458bc87bd4ce20641954a17f1c07cdf11`. The candidate is parent
`2e92a3d082b0a1e1eb0cdff48aa835919b2e1ddf` plus the
[archived source patch](release-v0.6.0/candidate.patch.gz). Its metadata explicitly
records a dirty working tree and patch digest; this is not a committed final
release-candidate gate. Generated evidence is excluded from the patch to avoid
including the archive in itself. Subsequent changes to test fixtures and
reporting do not change the benchmarked SDK implementation. The
[validated source patch](release-v0.6.0/validated-candidate.patch.gz) records the
final working tree after those updates.

Both builds used GCC 15.3.0, C++20, Release optimization, the same common
dependencies, equivalent fixture work and storage, and one Linux AMD EPYC 9V74
host. It exposes three affinity CPUs with a **two-core cgroup quota** and an
**8 GiB memory limit**. Dependency prefixes supplied gRPC 1.51.1, Protobuf
3.21.12, libcurl 8.23.0 and PostgreSQL 16.15; the candidate additionally used
Boost 1.92.0 and simdjson 4.6.11. The
[dependency inventory](release-v0.6.0/dependencies.json) and
[host limits](release-v0.6.0/host.json) retain the exact settings.

## Real-wire results

The corrected candidate completed **366 rows / 92,160 measured operations with
zero errors**: 324 wire rows, 36 public-client rows and six socket-resource rows.
Wire cases cover HTTP+JSON, JSON-RPC and gRPC, unary task creation and finite
streaming, c1/c16/c64, both stores, and three independent repetitions of 256
operations. Each coordinate starts a fresh SUT/schema. Variant order rotates
between repetitions. One candidate wire driver exercises all three servers:
v0.5.2 synchronous, candidate synchronous and candidate async.

The table summarizes the geometric mean of the six coordinate medians per
store/transport (two workloads × three concurrency levels). It does not replace
[the complete coordinate table](release-v0.6.0/matrix.md).

| Store / transport | v0.5.2 sync ops/s | Candidate sync ops/s | Sync delta | Candidate async ops/s |
|---|---:|---:|---:|---:|
| inmemory/http_json | 5759.1 | 6062.5 | +5.3% | 3935.9 |
| inmemory/jsonrpc | 4407.8 | 5315.8 | +20.6% | 3655.4 |
| inmemory/grpc | 6353.7 | 6277.4 | -1.2% | 6131.7 |
| postgres/http_json | 1933.8 | 2113.0 | +9.3% | 1542.8 |
| postgres/jsonrpc | 1589.0 | 1857.9 | +16.9% | 1422.8 |
| postgres/grpc | 1862.6 | 2012.1 | +8.0% | 1562.8 |

All eight adverse short-run alerts in synchronous throughput or p95 received
five alternating repetitions of **4,096 samples** each: another **327,680
operations with zero errors**. The worst confirmed throughput change was
**-3.9%**; the worst p95 change was **+1.5%**. See the
[confirmation coordinates](release-v0.6.0/confirmation.md). The 10% short-run
investigation trigger is a diagnostic choice; existing benchmark thresholds
were not changed. Short-run p99 values are retained but have limited statistical
power and are not a new release gate.

Async server throughput was often lower for this immediately ready fixture.
Coroutine scheduling and bounded blocking adapters have a measurable cost;
these results support bounded resource use, not universal async throughput
improvements. The separate public-client cases compare synchronous and opt-in
async `ListTasks` clients against the combined async server, not historical
client implementations.

Raw [matrix JSON](release-v0.6.0/matrix.json.gz) and
[confirmation JSON](release-v0.6.0/confirmation.json.gz) include each repetition,
operation counts, errors, throughput, first-event and completion p50/p95/p99,
CPU, sampled RSS and thread counts, full parent commits, binary/runner digests
and benchmark parameters. Unary cases have no first-event dimension. Wire
warmup is zero; each client-only case performs one unmeasured warmup call.
The PostgreSQL pool size is 16 in the wire/client matrix. Client construction
and fixture setup are outside operation latency; lazy
connection establishment can be included in early calls. Process-resource
sampling also includes driver setup.

## Resource, cancellation and push checks

At 64/256/1024 idle sockets and incomplete request headers, the event server held
all requested connections, sampled **21 threads with zero thread growth**, and
peaked at **25,220 KiB RSS**. Idle CPU was **0.5–1% of one core**, including the
SUT fixture's signal-check loop. Every process shut down cleanly with clients
still connected. Extra discovery probes succeeded below capacity; at the default
1024-connection cap an extra probe may be rejected.

The resource smoke requires at most 128 process threads, 512 MiB sampled RSS and
shutdown within 10 seconds. The two-second dwell is not proof of a long-term
memory plateau. `/proc` is sampled every 20 ms; CPU uses process time divided by
wall time, and sampled peaks can miss short-lived maxima. Very short client
runs can finish between samples. Database CPU is not included in SUT CPU.

In-memory c64 HTTP+JSON/JSON-RPC wire runs sampled 21 async-server threads versus
66–73 synchronous-server threads. These counts include runtime/transport workers.
The [23 event HTTP integration tests](release-v0.6.0/event-http.txt) passed,
including slow readers, pending-write cancellation, configured queue/buffer
limits, connection isolation and shutdown races. Internal queue occupancy is
not exposed by the process benchmark.

Real-wire push delivery at c1/c16/c64 reported zero errors. At c64, synchronous
delivery grew from 7 to 74 threads; async delivery grew from 7 to 10. The
[raw CSV](release-v0.6.0/push-delivery.csv) is a controlled pending-delivery test,
not a sustained production delivery-rate comparison. Process counts include
controller, recipient and caller threads; synchronous mode supplies one waiting
caller thread per delivery.

PostgreSQL store I/O remains **synchronous**. Coroutine fixtures use bounded
blocking adapters. Additional phase samples (default pool size 4, 64 operations
per c1/c16/c64 coordinate) reported zero errors and attribute task upsert,
push-config lookup and pool waiting. The c64 medians were 78.7 ms pool wait,
3.06 ms upsert and 0.47 ms lookup. These diagnostic samples ran while builds were
active and must not be used as before/after throughput evidence. Raw phases:
[c1](release-v0.6.0/postgres-phases-c1.json),
[c16](release-v0.6.0/postgres-phases-c16.json),
[c64](release-v0.6.0/postgres-phases-c64.json).

## Narrow regression fixes

Debug validation reproduced a deadline-completion stall on the host's standard
library configuration. The runtime retains `steady_clock::time_point::max()`
markers until callback cleanup. Converting that marker into a system-clock
timed wait overflowed and produced immediate spurious wakes while starving
cleanup. The timer now waits for notification when only those markers remain.
Unit coverage includes an unbounded call followed by expiry; real HTTP coverage
requires another request to complete after timeout. Existing failing deadline
checks pass after the fix, with no timeout changes.

The expanded TSan profile now builds the already-labelled async-client and
async-push unit targets. It also exposed a test assumption: a 30 ms delivery
could expire during first-use serialization before a fake requester existed.
The test requires one deadline completion and cancellation whenever initiation
occurred, without assuming initiation won that race or changing the deadline.

An initial GCC TSan run crashed nine tests in Protobuf map code. The
[standalone program](release-v0.6.0/protobuf-map-probe.cpp), which contains no
SDK code, reproduces the [same crash](release-v0.6.0/protobuf-map-gcc-tsan.txt.gz).
Protobuf 3.21's `map_field.h` adds fields under GCC's `__SANITIZE_THREAD__`;
those instrumented headers do not match the ordinary prebuilt library. The
identical program passes with Clang 19 TSan. The
[initial failure log](release-v0.6.0/tsan-gcc-failures.txt.gz) is retained. No
sanitizer suppression, Protobuf header patch or SDK workaround was applied.

## Validation and remaining release gates

- ASan/UBSan: **88 selected checks passed**, with leak detection enabled;
  [configuration](release-v0.6.0/asan-configuration.json) and
  [results](release-v0.6.0/asan-ubsan.txt).
- Clang 19 TSan: **398 checks passed**, without suppressions;
  [configuration](release-v0.6.0/tsan-configuration.json) and
  [results](release-v0.6.0/tsan-clang.txt).
- HTTP interoperability: six independent Python-wire-fixture scenarios passed
  for send, subscribe and cancellation across HTTP+JSON and JSON-RPC, plus six
  focused stream-gap checks against the installed SDK;
  [scenario output](release-v0.6.0/http-interop.txt).
- The final mandatory performance smoke completed **54 rows with zero errors**;
  [raw results](release-v0.6.0/perf-smoke-results.json.gz) and
  [output](release-v0.6.0/perf-smoke.txt).
- Canonical `./scripts/verify_changes.sh`: **passed**, with Asio and PostgreSQL
  enabled. Formatting, build and both strict Clang 19 lint profiles passed
  (84 source and 104 test translation units selected).
  CTest reported **1,175 passed, 3 skipped and zero failed** out of 1,178 checks.
  Two skips are HTTP bindings of a gRPC-only TLS case; the third is the HTTP+JSON
  instance of a JSON-RPC-only envelope case. These transport-scope exclusions are
  defined by the suite. [Full output](release-v0.6.0/verify-changes.txt.gz) and
  [configuration](release-v0.6.0/verify-configuration.json) retain the result.
- Installed synchronous/async examples and clean FetchContent checks passed.
  vcpkg manifest formatting, default and
  combined feature resolution, source-patch application and candidate version
  stamping were checked. The Linux combined-feature install built the common
  dependencies, then stopped because the enforced network policy blocks both
  PostgreSQL source-download hosts. The Asio-only Linux package installed and
  built/ran external hello, async-unary, coroutine, native-gRPC and event-HTTP
  examples with GCC 14, vcpkg gRPC 1.84.0, Protobuf 7.36.2 and simdjson 5.1.1.
  The separate default package passed hello/async-unary smoke checks with no
  Boost headers installed. Windows/macOS builds remain pending
  in the `Installed package consumers` workflow. It covers default, `asio`, and
  combined `asio,postgres-store` packages and uses the existing LLVM/libc++ setup
  for macOS Asio builds.
- `pip-audit` found no known vulnerabilities in installed TCK Python dependencies;
  the local TCK package itself is not on PyPI and was skipped. OSV Scanner could
  not parse the vcpkg manifests. That attempt is not a passing C++ vulnerability
  scan; supported C++ dependency scanning remains a release check.

The [Linux vcpkg inventory](release-v0.6.0/vcpkg-linux.json.gz) records its full
registry commit, compiler, release-only triplet, installed package ABIs and
consumer names. Raw logs cover [default install](release-v0.6.0/vcpkg-install-sync.txt.gz),
[default consumers](release-v0.6.0/vcpkg-consumers-sync.txt.gz),
[Asio install](release-v0.6.0/vcpkg-install-asio.txt.gz),
[Asio consumers](release-v0.6.0/vcpkg-consumers-asio.txt.gz) and the
[blocked combined install](release-v0.6.0/vcpkg-install.txt.gz).
The async-unary `--smoke` intentionally targets an unused loopback port and
succeeds when its async error callback completes; it prints that expected
connection error. Successful public-client wire calls are measured separately
in the matrix above.

Unmodified upstream TCK `104a7b4ae0d83f65c075e3793e322953a830eaba`
reported **242 passed, 4 failed and 17 skipped** for all four initial
sync/async × in-memory/PostgreSQL runs. The corrected candidate's async
PostgreSQL rerun reproduced the same result. The failures are
`HTTP_JSON-ERR-001`'s media-type assertion and `CORE-SEND-003` on all three
transports, documented in the [conformance discussion](../compatibility/conformance.md).
The mandatory conformance gate is **not passed**. The repository's
[AGENTS.md](https://github.com/MisterVVP/a2a-cpp/blob/release/v0.6.0/AGENTS.md#mandatory-tck-conformance-gate)
prohibits committing/pushing code until that gate is satisfied. Assertions, suite
exit status and thresholds were not changed.

Raw TCK reports: [sync in-memory](release-v0.6.0/tck-sync-inmemory.json.gz),
[sync PostgreSQL](release-v0.6.0/tck-sync-postgres.json.gz),
[async in-memory](release-v0.6.0/tck-async-inmemory.json.gz),
[async PostgreSQL](release-v0.6.0/tck-async-postgres.json.gz), and
[corrected async PostgreSQL](release-v0.6.0/tck-final-async-postgres.json.gz).

## Reproduce

Build v0.5.2 and the candidate in separate directories on the same host with
`-DCMAKE_BUILD_TYPE=Release`, the same dependency prefixes, testing and PostgreSQL
enabled. Enable `A2A_ENABLE_ASIO=ON` in the candidate. Build baseline `tck_sut`,
and candidate `tck_sut`, `tck_async_sut`, `a2a_wire_performance_driver` and
`async_unary_benchmark`.

```bash
export A2A_TEST_POSTGRES_DSN=postgresql://a2a@127.0.0.1:5432/a2a
python3 tests/performance/release_validation.py \
  --baseline-build work/build-baseline --candidate-build work/build-release \
  --requests 256 --repetitions 3 --output work/release-validation
```

For longer comparisons use `--requests 4096 --repetitions 5`. Use an otherwise
idle host and retain all raw rows, including failures. The runner requires Linux
`/proc`; non-Linux checks belong in the platform CI jobs. The manual
`Integrated release regression` workflow retains the same matrix and raw artifacts.

Initial pre-fix measurements are retained separately as
[pre-fix matrix](release-v0.6.0/matrix-pre-fix.json.gz),
[pre-fix confirmation](release-v0.6.0/confirmation-pre-fix.json.gz) and
[pre-fix source](release-v0.6.0/candidate-pre-fix.patch.gz). An ephemeral-port
collision and a later full-filesystem attempt were excluded from accepted
comparisons; their partial logs remain in the local validation workspace.
