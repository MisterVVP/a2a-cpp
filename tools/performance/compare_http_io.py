#!/usr/bin/env python3
"""Linux before/after HTTP I/O measurements; SDK wire-driver output is preserved."""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import platform
import socket
import statistics
import subprocess
import threading
import time

HOST = "127.0.0.1"
PORT = 28120
IDLE_CONNECTIONS = 128
CONCURRENCIES = (1, 64)
REPETITIONS = 3
DURATION = 2.0
WARMUP = 0.5
SAMPLE_SECONDS = 0.02
SCENARIOS = ("SendMessage_CreateTask", "GetTask_ExistingTask", "SendStreamingMessage_FiniteStream")
HEADER_END = b"\r\n\r\n"
POST_PATH = "/a2a/message:send"
THROUGHPUT_TOLERANCE = 0.10
TAIL_RELATIVE_TOLERANCE = 0.20
TAIL_ABSOLUTE_TOLERANCE_MS = 0.1


def receive_response(client):
    data = b""
    while HEADER_END not in data:
        part = client.recv(4096)
        if not part:
            raise ConnectionError("HTTP header EOF")
        data += part
    head, body = data.split(HEADER_END, 1)
    size = next(int(line.split(b":", 1)[1]) for line in head.split(b"\r\n") if line.lower().startswith(b"content-length:"))
    while len(body) < size:
        part = client.recv(4096)
        if not part:
            raise ConnectionError("HTTP body EOF")
        body += part
    if not head.startswith(b"HTTP/1.1 200"):
        raise AssertionError(head)


def connection(close=False):
    client = socket.create_connection((HOST, PORT), timeout=10)
    body = json.dumps({"message": {"messageId": "io-comparison", "role": "ROLE_USER", "parts": [{"text": "benchmark"}]}}).encode()
    headers = [f"POST {POST_PATH} HTTP/1.1", "Host: localhost", "Content-Type: application/json", "A2A-Version: 1.0",
               "A2A-Extensions: urn:a2a:tck:required-extension", f"Content-Length: {len(body)}",
               f"Connection: {'close' if close else 'keep-alive'}"]
    client.sendall("\r\n".join(headers).encode() + HEADER_END + body)
    receive_response(client)
    return client


def process_resources(pid):
    root = Path(f"/proc/{pid}")
    stat = (root / "stat").read_text().split(")", 1)[1].split()
    ticks = int(stat[11]) + int(stat[12])
    status = dict(line.split(":", 1) for line in (root / "status").read_text().splitlines() if ":" in line)
    return {"cpu_ticks": ticks, "rss_kib": int(status["VmRSS"].split()[0]),
            "virtual_kib": int(status["VmSize"].split()[0]), "threads": int(status["Threads"])}


class Sampler:
    def __init__(self, pid):
        self.pid = pid
        self.samples = []
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run)

    def run(self):
        while not self.stop.is_set():
            try:
                self.samples.append(process_resources(self.pid))
            except (OSError, KeyError):
                return
            self.stop.wait(SAMPLE_SECONDS)

    def finish(self, elapsed):
        self.stop.set()
        self.thread.join()
        first, last = self.samples[0], self.samples[-1]
        return {"server_cpu_percent": 100 * (last["cpu_ticks"] - first["cpu_ticks"]) / os.sysconf("SC_CLK_TCK") / elapsed,
                "peak_server_rss_kib": max(row["rss_kib"] for row in self.samples),
                "peak_server_virtual_kib": max(row["virtual_kib"] for row in self.samples),
                "peak_server_threads": max(row["threads"] for row in self.samples)}


def churn(concurrency):
    stop_at = time.monotonic() + DURATION
    def worker():
        latencies, errors = [], 0
        while time.monotonic() < stop_at:
            started = time.perf_counter()
            try:
                with connection(close=True):
                    pass
                latencies.append((time.perf_counter() - started) * 1000)
            except (OSError, AssertionError):
                errors += 1
        return latencies, errors
    started = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as pool:
        results = list(pool.map(lambda _: worker(), range(concurrency)))
    elapsed = time.perf_counter() - started
    values = sorted(value for latencies, _ in results for value in latencies)
    def percentile(percent):
        return values[min(len(values) - 1, int((len(values) - 1) * percent))] if values else 0
    return {"scenario": "ConnectionChurn_SendMessage", "operations": len(values), "errors": sum(e for _, e in results),
            "throughput_ops_per_sec": len(values) / elapsed, "latency_ms": {"p50": percentile(.5), "p95": percentile(.95), "p99": percentile(.99)}}


def measure(args, version, binary, backend, concurrency, workload, repetition):
    env = os.environ.copy()
    env["A2A_TCK_STORE_BACKEND"] = backend
    env["A2A_TCK_POSTGRES_POOL_SIZE"] = "64"
    if backend == "postgres":
        env["A2A_TCK_POSTGRES_DSN"] = os.environ["A2A_TEST_POSTGRES_DSN"]
        env["A2A_TCK_POSTGRES_SCHEMA"] = f"io_{args.run_id}_{version}_{repetition}_{concurrency}_{workload}"
    process = subprocess.Popen([binary, f"{HOST}:{PORT}"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    clients = []
    sampler = None
    try:
        deadline = time.monotonic() + 20
        while True:
            try:
                with connection():
                    break
            except OSError:
                if process.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("SUT startup failed")
                time.sleep(.02)
        if workload == "idle":
            clients = [connection() for _ in range(IDLE_CONNECTIONS)]
        sampler = Sampler(process.pid)
        sampler.thread.start()
        started = time.perf_counter()
        if workload == "churn":
            rows = [churn(concurrency)]
        else:
            command = [args.driver, "--transport", "http_json", "--store-backend", backend, "--host", HOST, "--port", str(PORT),
                       "--requests", "1000", "--concurrency", str(concurrency), "--warmup-seconds", str(WARMUP),
                       "--duration-seconds", str(DURATION), "--scenarios", ",".join(SCENARIOS if workload == "wire" else SCENARIOS[:1])]
            result = subprocess.run(command, capture_output=True, text=True, timeout=90, check=True)
            rows = json.loads(result.stdout)
        resources = sampler.finish(time.perf_counter() - started)
        for client in clients:
            client.close()
        clients.clear()
        process.terminate()
        output, _ = process.communicate(timeout=20)
        if process.returncode:
            raise RuntimeError(output.decode())
        for row in rows:
            row.update(version=version, repetition=repetition, workload=workload, store_backend=backend, concurrency=concurrency,
                       idle_connections=IDLE_CONNECTIONS if workload == "idle" else 0, resources=resources,
                       server_diagnostics=output.decode())
        return rows
    finally:
        if sampler is not None:
            sampler.stop.set()
            sampler.thread.join()
        for client in clients:
            client.close()
        if process.poll() is None:
            process.kill()
            process.wait()


def aggregate(rows):
    groups = {}
    for row in rows:
        key = (row["store_backend"], row["concurrency"], row["workload"], row["scenario"])
        groups.setdefault(key, {}).setdefault(row["version"], []).append(row)
    result = []
    for key, versions in sorted(groups.items()):
        summary = dict(zip(("store_backend", "concurrency", "workload", "scenario"), key))
        for version, values in versions.items():
            summary[version] = {"throughput_ops_per_sec": statistics.median(v["throughput_ops_per_sec"] for v in values),
                                "latency_ms": {p: statistics.median(v["latency_ms"][p] for v in values) for p in ("p50", "p95", "p99")},
                                "errors": sum(int(v["errors"]) for v in values),
                                "resources": {name: statistics.median(v["resources"][name] for v in values) for name in values[0]["resources"]}}
        before, after = summary["before"], summary["after"]
        summary["throughput_change_percent"] = 100 * (after["throughput_ops_per_sec"] / before["throughput_ops_per_sec"] - 1)
        summary["within_tolerance"] = (summary["throughput_change_percent"] >= -100 * THROUGHPUT_TOLERANCE and
            all(after["latency_ms"][p] - before["latency_ms"][p] <= max(TAIL_ABSOLUTE_TOLERANCE_MS, before["latency_ms"][p] * TAIL_RELATIVE_TOLERANCE) for p in ("p95", "p99")) and not after["errors"])
        result.append(summary)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("before", "after", "driver", "output"):
        parser.add_argument(f"--{name}", required=True)
    args = parser.parse_args()
    args.run_id = str(time.time_ns())
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    metadata = {"platform": platform.platform(), "cpu_model": next(line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines() if line.startswith("model name")),
                "cpu_quota": Path("/sys/fs/cgroup/cpu.max").read_text().strip(),
                "memory_limit_bytes": Path("/sys/fs/cgroup/memory.max").read_text().strip(),
                "run_id": args.run_id, "cpu_count": os.cpu_count(), "cpu_affinity_count": len(os.sched_getaffinity(0)), "duration_seconds": DURATION,
                "warmup_seconds": WARMUP, "repetitions": REPETITIONS, "concurrency": CONCURRENCIES, "postgres_pool_size": 64,
                "tolerances": {"throughput_relative": THROUGHPUT_TOLERANCE, "tail_latency_relative": TAIL_RELATIVE_TOLERANCE, "tail_latency_absolute_ms": TAIL_ABSOLUTE_TOLERANCE_MS}}
    (output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    rows = []
    for repetition in range(REPETITIONS):
        for backend in ("inmemory", "postgres"):
            for concurrency in CONCURRENCIES:
                for workload in ("wire", "churn", "idle"):
                    versions = [("before", args.before), ("after", args.after)]
                    if repetition % 2:
                        versions.reverse()
                    for version, binary in versions:
                        print(version, backend, concurrency, workload, repetition, flush=True)
                        rows.extend(measure(args, version, binary, backend, concurrency, workload, repetition))
                        (output / "samples.json").write_text(json.dumps(rows, indent=2) + "\n")
    (output / "comparison.json").write_text(json.dumps(aggregate(rows), indent=2) + "\n")


if __name__ == "__main__":
    main()
