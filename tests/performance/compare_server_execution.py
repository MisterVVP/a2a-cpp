# SPDX-License-Identifier: Apache-2.0
"""Interleave server variants using one Release wire driver and one workload per SUT."""

import argparse
import itertools
import json
import os
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from performance_runner import find_available_sut_port, run_command_json, wait_for_port

GET_TASK = "GetTask_ExistingTask"
LIST_TASKS = "ListTasks_NoPagination"
FINITE_SSE = "SendStreamingMessage_FiniteStream"
FIXTURE_ID = "benchmark-task"
FIXTURE_MODES = {"listener", "immediate"}
HOST = "127.0.0.1"


def pinned(cpus, *command):
    return ["taskset", "--cpu-list", cpus, *command] if cpus else list(command)


def sample(args, variant, transport, scenario, client, delay, concurrency, repetition):
    name, mode, binary = variant
    port = find_available_sut_port(HOST)
    target_port = port + int(transport == "grpc")
    coordinates = {"variant": name, "server_mode": mode, "transport": transport, "scenario": scenario,
                   "client_mode": client, "delay_ms": delay, "concurrency": concurrency, "repetition": repetition}
    log_path = args.output.parent / (
        f"{args.output.stem}-{name}-{transport}-{scenario}-{client}-{delay}-{concurrency}-{repetition}.log")
    environment = dict(os.environ, A2A_TCK_STORE_BACKEND="inmemory", A2A_PERF_SERVER_MODE=mode,
                       A2A_PERF_GET_TASK_DELAY_MS=str(delay), A2A_PERF_HTTP_PENDING_LIMIT=str(args.http_pending_limit))
    with log_path.open("w", encoding="utf-8") as log:
        server = subprocess.Popen(pinned(args.server_cpus, str(Path(binary).resolve()), f"{HOST}:{port}"),
                                  env=environment, stdout=log, stderr=subprocess.STDOUT)
    try:
        wait_for_port(HOST, target_port, server, log_path)
        if mode != "sync" and f"A2A_PERFORMANCE_MODE server={mode} " not in log_path.read_text(encoding="utf-8"):
            raise ValueError(f"{name} did not confirm the requested server mode")
        resources = {}
        command = pinned(args.client_cpus, str(args.driver.resolve()), "--transport", transport,
                         "--store-backend", "inmemory", "--host", HOST, "--port", str(port),
                         "--requests", str(args.requests), "--concurrency", str(concurrency),
                         "--warmup-seconds", str(args.warmup_seconds), "--duration-seconds", str(args.duration_seconds),
                         "--client-mode", client, "--scenarios", scenario)
        if mode in FIXTURE_MODES:
            command.extend(("--fixed-task-id", FIXTURE_ID))
        (result,) = run_command_json(command, args.timeout_seconds, name, log_path, server_pid=server.pid,
                                     resources=resources)
        expected_client = "observer" if scenario == FINITE_SSE else client
        expected = {"scenario": scenario, "transport": transport, "concurrency": concurrency,
                    "client_execution_mode": expected_client, "configured_requests": args.requests}
        if any(result.get(key) != value for key, value in expected.items()):
            raise ValueError(f"{name} returned a different workload from the requested one")
        return {**result, **coordinates, **resources, "driver_timeout": False}
    except (ValueError, OSError) as error:
        return {**coordinates, "failure": str(error),
                "driver_timeout": isinstance(error.__cause__, subprocess.TimeoutExpired)}
    finally:
        server.terminate()
        try:
            server.wait(timeout=args.timeout_seconds)
        except subprocess.TimeoutExpired as error:
            server.kill()
            server.wait()
            raise RuntimeError(f"{name} failed to shut down. See {log_path}") from error


def schedule(args):
    coordinates = list(itertools.product(args.transports, args.scenarios, args.clients, args.delay_ms,
                                         args.concurrency))
    for repetition in range(1, args.repetitions + 1):
        coordinates.reverse()
        variants = args.server if repetition % 2 else list(reversed(args.server))
        for transport, scenario, client, delay, concurrency in coordinates:
            if (client == "async" and scenario == FINITE_SSE) or (delay and scenario != GET_TASK):
                continue
            for variant in variants:
                if variant[1] in FIXTURE_MODES and (transport != "http_json" or scenario != GET_TASK or delay):
                    continue
                yield variant, transport, scenario, client, delay, concurrency, repetition


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--server", action="append", nargs=3, metavar=("LABEL", "MODE", "BINARY"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--server-cpus")
    parser.add_argument("--client-cpus")
    parser.add_argument("--concurrency", nargs="+", type=int, default=[1, 16, 64, 256])
    parser.add_argument("--transports", nargs="+", choices=["http_json", "jsonrpc", "grpc"],
                        default=["http_json", "jsonrpc", "grpc"])
    parser.add_argument("--scenarios", nargs="+", choices=[GET_TASK, LIST_TASKS, FINITE_SSE],
                        default=[GET_TASK, LIST_TASKS, FINITE_SSE])
    parser.add_argument("--clients", nargs="+", choices=["sync", "async"], default=["sync", "async"])
    parser.add_argument("--delay-ms", nargs="+", type=int, default=[0])
    parser.add_argument("--http-pending-limit", type=int, default=0)
    parser.add_argument("--requests", type=int, default=30000)
    parser.add_argument("--warmup-seconds", type=float, default=0.25)
    parser.add_argument("--duration-seconds", type=float, default=1.0)
    parser.add_argument("--timeout-seconds", type=float, default=30.0)
    parser.add_argument("--repetitions", type=int, default=3)
    args = parser.parse_args()
    if min(args.concurrency) < 1 or max(args.concurrency) > args.requests or args.repetitions < 1:
        parser.error("Require at least one repetition and enough requests to start every lane")
    if min(args.delay_ms) < 0 or args.http_pending_limit < 0 or args.timeout_seconds <= 0:
        parser.error("Delays and pending limits must be nonnegative and the timeout must be positive")
    if args.warmup_seconds < 0 or args.duration_seconds < 0:
        parser.error("Warmup and measurement durations must be nonnegative")
    names = set()
    for name, mode, _ in args.server:
        if not name or Path(name).name != name or name in names or mode not in {"sync", "async", *FIXTURE_MODES}:
            parser.error("Each server must have a distinct label and mode sync, async, listener or immediate")
        names.add(name)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    metadata = {**vars(args), "driver": str(args.driver), "output": str(args.output)}
    failures = False
    with args.output.open("w", encoding="utf-8") as output:
        print(json.dumps({"configuration": metadata}), file=output)
        for coordinate in schedule(args):
            row = sample(args, *coordinate)
            print(json.dumps(row), file=output, flush=True)
            failures |= "failure" in row or bool(row.get("errors"))
    return int(failures)


if __name__ == "__main__":
    raise SystemExit(main())
