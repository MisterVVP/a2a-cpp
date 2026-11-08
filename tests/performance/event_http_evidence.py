# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)
"""Compare the shared synchronous SUT and installed event HTTP runtime on Linux.

Both SUTs must be built in the same configuration. Uses identical discovery and
finite SSE workloads, in-memory stores, headers, request counts and persistence.
The coroutine SUT uses the same fixture backend through its bounded adapter.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import http.client
import json
import math
import os
from pathlib import Path
import signal
import socket
import subprocess
import threading
import time

HOST = "127.0.0.1"
CARD_PATH = "/.well-known/agent-card.json"
STREAM_PATH = "/a2a/message:stream"
HEADERS = {"A2A-Version": "1.0", "A2A-Extensions": "urn:a2a:tck:required-extension",
           "Content-Type": "application/json"}
TIMEOUT = 10
SAMPLE_INTERVAL = 0.02
IDLE_SECONDS = 1


def free_port_pair():
    for _ in range(100):
        with socket.socket() as first, socket.socket() as second:
            first.bind((HOST, 0))
            port = first.getsockname()[1]
            try:
                second.bind((HOST, port + 1))
                return port
            except OSError:
                pass
    raise RuntimeError("No adjacent free port pair")


def process_sample(pid):
    status = {}
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        name, _, value = line.partition(":")
        status[name] = value.strip()
    # The executable name in parentheses can contain spaces.
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    cpu = (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")
    return {"rss_kib": int(status["VmRSS"].split()[0]), "threads": int(status["Threads"]), "cpu": cpu}


def percentile(values, fraction):
    return sorted(values)[min(len(values) - 1, math.ceil(len(values) * fraction) - 1)] if values else None


def exercise(port, worker, count, streaming, ready):
    connection = http.client.HTTPConnection(HOST, port, timeout=TIMEOUT)
    connection.connect()
    ready.wait(timeout=TIMEOUT)
    completions, first_events, errors = [], [], 0
    for index in range(count):
        body = json.dumps({"message": {"messageId": f"event-http-{worker}-{index}", "role": "user",
                                      "parts": [{"text": "event HTTP evidence"}]}}) if streaming else None
        start = time.perf_counter()
        try:
            connection.request("POST" if streaming else "GET", STREAM_PATH if streaming else CARD_PATH,
                               body=body, headers=HEADERS)
            response = connection.getresponse()
            first = None
            if streaming:
                while line := response.readline():
                    if line.startswith(b"event: error"):
                        errors += 1
                    if line.startswith(b"data:"):
                        if first is None:
                            first = (time.perf_counter() - start) * 1000
                        event = json.loads(line.removeprefix(b"data:"))
                        errors += int("error" in event)
                if first is None:
                    errors += 1
                else:
                    first_events.append(first)
            else:
                if not response.read():
                    errors += 1
            errors += int(response.status != 200)
            completions.append((time.perf_counter() - start) * 1000)
        except (OSError, http.client.HTTPException):
            errors += 1
            connection.close()
    connection.close()
    return completions, first_events, errors


def run_case(binary, mode, concurrency, requests, streaming, directory, idle=False):
    port = free_port_pair()
    environment = dict(os.environ, A2A_TCK_STORE_BACKEND="inmemory", A2A_TCK_TRANSPORTS="grpc,jsonrpc,http_json",
                       A2A_TCK_REQUIRED_EXTENSIONS="urn:a2a:tck:required-extension")
    log_path = directory / f"{mode}-{concurrency}-{'idle' if idle else 'sse' if streaming else 'unary'}.log"
    with log_path.open("w") as log:
        process = subprocess.Popen([str(binary.resolve()), f"{HOST}:{port}"], env=environment,
                                   stdout=log, stderr=log)
        clients = []
        try:
            deadline = time.monotonic() + TIMEOUT
            while True:
                try:
                    with socket.create_connection((HOST, port), timeout=1):
                        break
                except OSError:
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError(log_path.read_text())
                    time.sleep(SAMPLE_INTERVAL)
            samples = []
            finished = threading.Event()
            def sample():
                while not finished.wait(SAMPLE_INTERVAL):
                    samples.append(process_sample(process.pid))
            sampler = threading.Thread(target=sample)
            sampler.start()
            before = process_sample(process.pid)
            start = time.perf_counter()
            if idle:
                clients = [socket.create_connection((HOST, port), timeout=TIMEOUT) for _ in range(concurrency)]
                time.sleep(IDLE_SECONDS)
                results = []
            else:
                ready = threading.Barrier(concurrency)
                with ThreadPoolExecutor(max_workers=concurrency) as workers:
                    results = list(workers.map(lambda worker: exercise(port, worker, requests // concurrency, streaming, ready),
                                              range(concurrency)))
            elapsed = time.perf_counter() - start
            after = process_sample(process.pid)
            finished.set()
            sampler.join()
            samples.extend([before, after])
            completions = [value for result in results for value in result[0]]
            events = [value for result in results for value in result[1]]
            return {"runtime": mode, "workload": "idle" if idle else "finite_sse" if streaming else "agent_card",
                    "concurrency": concurrency, "requests": len(completions),
                    "errors": sum(result[2] for result in results),
                    "throughput_rps": len(completions) / elapsed,
                    "completion_p95_ms": percentile(completions, 0.95),
                    "completion_p99_ms": percentile(completions, 0.99),
                    "first_event_p95_ms": percentile(events, 0.95), "first_event_p99_ms": percentile(events, 0.99),
                    "cpu_percent": 100 * (after["cpu"] - before["cpu"]) / elapsed,
                    "peak_rss_kib": max(row["rss_kib"] for row in samples),
                    "peak_threads": max(row["threads"] for row in samples), "seconds": elapsed}
        finally:
            for client in clients:
                client.close()
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=TIMEOUT)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise RuntimeError(f"Shutdown timed out: {log_path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--event", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--requests", type=int, default=512)
    args = parser.parse_args()
    if args.requests < 64 or args.requests % 64:
        parser.error("--requests must be a positive multiple of 64")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    rows = []
    for mode, binary in (("synchronous", args.baseline), ("event", args.event)):
        for concurrency in (1, 16, 64):
            for streaming in (False, True):
                row = run_case(binary, mode, concurrency, args.requests, streaming, args.output.parent)
                rows.append(row)
                print(json.dumps(row), flush=True)
        rows.append(run_case(binary, mode, 64, 0, False, args.output.parent, idle=True))
    args.output.write_text(json.dumps({"results": rows}, indent=2) + "\n")
    if any(row["errors"] for row in rows):
        raise SystemExit("HTTP runtime evidence reported errors")


if __name__ == "__main__":
    main()
