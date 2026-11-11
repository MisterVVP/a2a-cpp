# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)
"""Linux integrated release evidence; run optimized baseline/candidate builds on one host.

Every wire coordinate starts a fresh SUT with identical work and store settings.
Retain results even on failure. No threshold or conformance result is inferred.
"""
import argparse
from contextlib import ExitStack, contextmanager
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import resource
import secrets
import signal
import socket
import statistics
import subprocess
import threading
import time

from event_http_evidence import HOST, TIMEOUT, SAMPLE_INTERVAL, process_sample

CONCURRENCIES = (1, 16, 64)
TRANSPORTS = ("http_json", "jsonrpc", "grpc")
SCENARIOS = ("SendMessage_CreateTask", "SendStreamingMessage_FiniteStream")
IDLE_COUNTS = (64, 256, 1024)
IDLE_SECONDS = 2
DRIVER_TIMEOUT = 120
THREAD_LIMIT = 128
RSS_LIMIT_KIB = 512 * 1024
EXTENSION = "urn:a2a:tck:required-extension"
EVIDENCE_PATH = "book/src/performance/release-v0.6.0/**"
REST_PATH = "/a2a"
JSONRPC_PATH = "/rpc"
HTTP_OK_PREFIX = b"HTTP/1.1 200"
SLOW_HEADERS = b"POST /a2a/message:send HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1024\r\n"
DISCOVERY_REQUEST = b"GET /.well-known/agent-card.json HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def git(source, *args):
    return subprocess.check_output(["git", "-C", str(source), *args], text=True).strip()


def source_patch(source):
    # Exclude generated evidence so archiving the tested source cannot include itself.
    return subprocess.check_output(["git", "-C", str(source), "diff", "--binary", "HEAD", "--", ".", ":!" + EVIDENCE_PATH])


def build_metadata(build):
    cache = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if not line.startswith(("#", "//")) and "=" in line:
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    source = Path(cache["CMAKE_HOME_DIRECTORY"])
    patch = source_patch(source)
    return {"commit": git(source, "rev-parse", "HEAD"), "source": str(source),
            "tracked_diff_excludes": [EVIDENCE_PATH], "tracked_diff_sha256": hashlib.sha256(patch).hexdigest(),
            "dirty": bool(git(source, "status", "--porcelain", "--untracked-files=no")),
            "build_type": cache["CMAKE_BUILD_TYPE"], "compiler": cache["CMAKE_CXX_COMPILER"],
            "compiler_version": subprocess.check_output([cache["CMAKE_CXX_COMPILER"], "--version"], text=True).splitlines()[0],
            "dependencies": {key: value for key, value in cache.items()
                             if key.endswith("_DIR") and any(name.lower() in key.lower() for name in ("Boost", "simdjson", "grpc", "protobuf"))},
            "binaries": {name: digest(build / "tests" / name) for name in
                         ("tck_sut", "tck_async_sut", "a2a_wire_performance_driver", "async_unary_benchmark")
                         if (build / "tests" / name).exists()}}


def listener_ports():
    # A bind(0) pair can collide with the readiness client's ephemeral port
    # before the SUT has bound its second listener. Stay below that OS range.
    ephemeral_start = int(Path("/proc/sys/net/ipv4/ip_local_port_range").read_text().split()[0])
    minimum = 1024
    for _ in range(100):
        port = minimum + secrets.randbelow(ephemeral_start - minimum - 1)
        with socket.socket() as http, socket.socket() as grpc:
            try:
                http.bind((HOST, port))
                grpc.bind((HOST, port + 1))
                return port
            except OSError:
                continue
    raise RuntimeError("No non-ephemeral listener port pair available")


@contextmanager
def server(binary, backend, directory, label):
    port = listener_ports()
    environment = dict(os.environ, A2A_TCK_STORE_BACKEND=backend,
                       A2A_TCK_TRANSPORTS=",".join(TRANSPORTS), A2A_TCK_REQUIRED_EXTENSIONS=EXTENSION,
                       A2A_TCK_POSTGRES_SCHEMA=f"release_{os.getpid()}_{time.monotonic_ns()}",
                       A2A_TCK_POSTGRES_POOL_SIZE="16")
    if backend == "postgres":
        environment["A2A_TCK_POSTGRES_DSN"] = os.environ["A2A_TEST_POSTGRES_DSN"]
    with (directory / (label + ".log")).open("w") as log:
        process = subprocess.Popen([str(binary), f"{HOST}:{port}"], env=environment, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + TIMEOUT
            while True:
                try:
                    with socket.create_connection((HOST, port), timeout=1), socket.create_connection((HOST, port + 1), timeout=1):
                        break
                except OSError:
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError(f"SUT not ready: {label}.log")
                    time.sleep(SAMPLE_INTERVAL)
            yield process, port
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
            try:
                code = process.wait(timeout=TIMEOUT)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise RuntimeError(f"SUT shutdown timed out: {label}") from None
            if code != 0:
                raise RuntimeError(f"SUT exited with {code}: {label}")


@contextmanager
def monitor(pid):
    samples = [process_sample(pid)]
    def capture():
        try:
            samples.append(process_sample(pid))
        except (FileNotFoundError, ProcessLookupError, KeyError):
            # A completed/reaped child no longer has a readable Linux status.
            pass
    stopped = threading.Event()
    def sample():
        while not stopped.wait(SAMPLE_INTERVAL):
            capture()
    worker = threading.Thread(target=sample)
    started = time.monotonic()
    worker.start()
    metrics = {}
    try:
        yield metrics
    finally:
        stopped.set()
        worker.join()
        capture()
        seconds = time.monotonic() - started
        metrics.update(seconds=seconds, peak_threads=max(s["threads"] for s in samples),
                       peak_rss_kib=max(s["rss_kib"] for s in samples),
                       cpu_seconds=samples[-1]["cpu"] - samples[0]["cpu"],
                       cpu_percent=100 * (samples[-1]["cpu"] - samples[0]["cpu"]) / seconds)


def run_driver(command, directory, label):
    with (directory / (label + "-driver.log")).open("w") as errors:
        process = subprocess.Popen([str(value) for value in command], stdout=subprocess.PIPE, stderr=errors, text=True)
        with monitor(process.pid) as metrics:
            try:
                output, _ = process.communicate(timeout=DRIVER_TIMEOUT)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()
                raise RuntimeError(f"Driver timed out: {label}") from None
        (directory / (label + "-driver.json")).write_text(output)
        if process.returncode:
            raise RuntimeError(f"Driver exited with {process.returncode}: {label}")
        return json.loads(output), metrics


def wire_case(binary, driver, backend, transport, concurrency, requests, scenario, directory, label):
    with server(binary, backend, directory, label) as (process, port):
        command = [driver, "--transport", transport, "--store-backend", backend,
                   "--host", HOST, "--port", port, "--concurrency", concurrency,
                   "--requests", requests, "--warmup-seconds", 0, "--duration-seconds", 0, "--scenarios", scenario]
        with monitor(process.pid) as metrics:
            rows, client_metrics = run_driver(command, directory, label)
        if len(rows) != 1 or rows[0]["errors"] or int(rows[0]["operations"]) != requests:
            raise RuntimeError(f"Expected one zero-error row with {requests} samples: {label}")
        return dict(rows[0], server_resources=metrics, client_resources=client_metrics)


def idle_case(binary, count, slow, directory, label):
    with ExitStack() as clients, server(binary, "inmemory", directory, label) as (process, port):
        time.sleep(SAMPLE_INTERVAL * 5)
        before = process_sample(process.pid)
        before_sockets = sum(path.readlink().as_posix().startswith("socket:") for path in Path(f"/proc/{process.pid}/fd").iterdir())
        for _ in range(count):
            client = clients.enter_context(socket.create_connection((HOST, port), timeout=TIMEOUT))
            if slow:
                client.sendall(SLOW_HEADERS)
        with monitor(process.pid) as metrics:
            # Keep sockets open through shutdown to cancel pending reads.
            time.sleep(IDLE_SECONDS)
            connected_sockets = sum(path.readlink().as_posix().startswith("socket:") for path in Path(f"/proc/{process.pid}/fd").iterdir()) - before_sockets
            response = b""
            try:
                with socket.create_connection((HOST, port), timeout=TIMEOUT) as probe:
                    probe.sendall(DISCOVERY_REQUEST)
                    response = probe.recv(1024)
            except OSError:
                if count < 1024:
                    raise
        # At the default 1024 connection limit, rejection of an extra probe is valid.
        errors = int(count < 1024 and not response.startswith(HTTP_OK_PREFIX))
        result = {"workload": "slow_headers" if slow else "idle", "connections": count,
                  "accepted_socket_growth": connected_sockets,
                  "errors": errors, "server_resources": metrics,
                  "thread_growth": metrics["peak_threads"] - before["threads"],
                  "rss_growth_kib": metrics["peak_rss_kib"] - before["rss_kib"]}
    if errors or connected_sockets < count or metrics["peak_threads"] > THREAD_LIMIT or metrics["peak_rss_kib"] > RSS_LIMIT_KIB:
        raise RuntimeError(f"Idle resource/health gate failed: {label}: {result}")
    return result


def comparison(rows):
    lines = ["| Store / transport / workload / c | Baseline sync rps | Candidate sync rps (delta) | Candidate async server rps | Sync p95 delta |",
             "|---|---:|---:|---:|---:|"]
    for store in ("inmemory", "postgres"):
        for transport in TRANSPORTS:
            for scenario in SCENARIOS:
                for concurrency in CONCURRENCIES:
                    matching = [row for row in rows if row.get("store_backend") == store and
                                row.get("transport") == transport and row.get("scenario") == scenario and
                                row.get("concurrency") == concurrency]
                    modes = {mode: [r for r in matching if r["runtime"] == mode] for mode in
                             ("baseline_sync", "candidate_sync", "candidate_async")}
                    if any(not value for value in modes.values()):
                        continue
                    rates = {mode: statistics.median(r["throughput_ops_per_sec"] for r in values) for mode, values in modes.items()}
                    tails = {mode: statistics.median(r["latency_ms"]["p95"] for r in values) for mode, values in modes.items()}
                    delta = 100 * (rates["candidate_sync"] / rates["baseline_sync"] - 1)
                    tail_delta = 100 * (tails["candidate_sync"] / tails["baseline_sync"] - 1)
                    short = "unary" if scenario == SCENARIOS[0] else "stream"
                    lines.append(f"| {store}/{transport}/{short}/c{concurrency} | {rates['baseline_sync']:.1f} | "
                                 f"{rates['candidate_sync']:.1f} ({delta:+.1f}%) | {rates['candidate_async']:.1f} | {tail_delta:+.1f}% |")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-build", type=Path, required=True)
    parser.add_argument("--candidate-build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--requests", type=int, default=256)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--stores", nargs="+", choices=("inmemory", "postgres"), default=["inmemory", "postgres"])
    args = parser.parse_args()
    if args.requests < max(CONCURRENCIES) or args.repetitions < 1:
        parser.error("requests must be >= 64 and repetitions >= 1")
    if "postgres" in args.stores and not os.environ.get("A2A_TEST_POSTGRES_DSN"):
        parser.error("PostgreSQL cases require A2A_TEST_POSTGRES_DSN")
    if platform.system() != "Linux":
        parser.error("process resource sampling requires Linux /proc")
    args.output.mkdir(parents=True, exist_ok=True)
    baseline = args.baseline_build.resolve()
    candidate = args.candidate_build.resolve()
    metadata = {"started_utc": datetime.now(timezone.utc).isoformat(), "host": platform.platform(),
                "cpu": platform.processor(), "available_cpus": len(os.sched_getaffinity(0)),
                "cgroup_limits": {name: Path("/sys/fs/cgroup", name).read_text().strip()
                                  for name in ("cpu.max", "memory.max", "pids.max")
                                  if Path("/sys/fs/cgroup", name).is_file()},
                "nofile_limit": resource.getrlimit(resource.RLIMIT_NOFILE), "requests": args.requests,
                "repetitions": args.repetitions, "warmup_seconds": 0, "postgres_pool_size": 16,
                "resource_sample_interval_seconds": SAMPLE_INTERVAL, "zero_error_expectation": True,
                "event_idle_limits": {"max_threads": THREAD_LIMIT, "max_rss_kib": RSS_LIMIT_KIB},
                "baseline": build_metadata(baseline), "candidate": build_metadata(candidate),
                "runner_sha256": digest(Path(__file__))}
    if metadata["baseline"]["build_type"] != "Release" or metadata["candidate"]["build_type"] != "Release":
        parser.error("both builds must use Release")
    patch = source_patch(Path(metadata["candidate"]["source"]))
    if hashlib.sha256(patch).hexdigest() != metadata["candidate"]["tracked_diff_sha256"]:
        raise RuntimeError("Candidate source changed while recording metadata")
    (args.output / "candidate.patch").write_bytes(patch)
    rows = []
    def save(row):
        rows.append(row)
        (args.output / "results.json").write_text(json.dumps({"metadata": metadata, "results": rows}, indent=2) + "\n")
        print(json.dumps(row), flush=True)
    variants = (("baseline_sync", baseline / "tests/tck_sut"),
                ("candidate_sync", candidate / "tests/tck_sut"),
                ("candidate_async", candidate / "tests/tck_async_sut"))
    for repetition in range(args.repetitions):
        # Rotate order to reduce systematic temperature/time bias across variants.
        ordered = variants[repetition % len(variants):] + variants[:repetition % len(variants)]
        for store in args.stores:
            for transport in TRANSPORTS:
                for concurrency in CONCURRENCIES:
                    for scenario in SCENARIOS:
                        for mode, binary in ordered:
                            label = f"r{repetition}-{mode}-{store}-{transport}-c{concurrency}-{scenario}"
                            row = wire_case(binary, candidate / "tests/a2a_wire_performance_driver", store, transport,
                                            concurrency, args.requests, scenario, args.output, label)
                            save(dict(row, runtime=mode, repetition=repetition))
    # Compare both public client execution paths against the combined async server.
    for store in args.stores:
        for transport in TRANSPORTS:
            for concurrency in CONCURRENCIES:
                for mode in ("sync", "async"):
                    label = f"client-{mode}-{store}-{transport}-c{concurrency}"
                    with server(candidate / "tests/tck_async_sut", store, args.output, label) as (process, port):
                        endpoint = f"{HOST}:{port + 1}" if transport == "grpc" else f"http://{HOST}:{port}" + (REST_PATH if transport == "http_json" else JSONRPC_PATH)
                        with monitor(process.pid) as metrics:
                            row, client_metrics = run_driver([candidate / "tests/async_unary_benchmark", transport,
                                                             endpoint, mode, args.requests, concurrency], args.output, label)
                        if row["errors"]:
                            raise RuntimeError(f"Async client errors: {label}")
                        save(dict(row, workload="client_list_tasks", store_backend=store,
                                  server_resources=metrics, client_resources=client_metrics))
    for count in IDLE_COUNTS:
        for slow in (False, True):
            label = f"event-{'slow' if slow else 'idle'}-{count}"
            save(idle_case(candidate / "tests/tck_async_sut", count, slow, args.output, label))
    (args.output / "comparison.md").write_text(comparison(rows))


if __name__ == "__main__":
    main()
