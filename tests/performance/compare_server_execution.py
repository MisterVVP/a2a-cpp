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
RAW_FIXTURE_MODES = {"listener", "immediate"}
PIPELINE_MODES = {"pipeline-sync", "pipeline-async"}
FIXTURE_MODES = RAW_FIXTURE_MODES | PIPELINE_MODES
ASYNC_MODES = {"async", "pipeline-async"}
HOST = "127.0.0.1"
TOPOLOGY_MARKER = "A2A_PERFORMANCE_TOPOLOGY "
TOPOLOGY_KEYS = {"ingress", "dispatcher_threads", "http_application_threads",
                 "http_network_threads", "http_pending_limit"}


def pinned(cpus, *command):
    return ["taskset", "--cpu-list", cpus, *command] if cpus else list(command)


def cpu_list(spec):
    cpus = set()
    for item in spec.split(","):
        first, separator, last = item.partition("-")
        start, stop = int(first), int(last if separator else first)
        if start < 0 or stop < start:
            raise ValueError(f"Invalid CPU list {spec}")
        cpus.update(range(start, stop + 1))
    return cpus


def cpu_allocation(server_spec, client_spec):
    server, client = cpu_list(server_spec), cpu_list(client_spec)
    affinity = os.sched_getaffinity(0)
    if not server or not client or server & client or not (server | client) <= affinity:
        raise ValueError("Server and client CPU lists must be disjoint subsets of current affinity")
    cores = {}
    for cpu in server | client:
        path = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list")
        cores[cpu] = tuple(sorted(cpu_list(path.read_text(encoding="utf-8").strip())))
    if len(set(cores.values())) != len(cores):
        raise ValueError("CPU lists must select different cores, including across server and client")
    quotas = []
    cgroup_root = Path("/sys/fs/cgroup")
    for line in Path("/proc/self/cgroup").read_text(encoding="utf-8").splitlines():
        if line.startswith("0::"):
            group = cgroup_root / line.removeprefix("0::/").lstrip("/")
            for directory in (group, *group.parents):
                limit = directory / "cpu.max"
                if cgroup_root in (directory, *directory.parents) and limit.is_file():
                    quota, period = limit.read_text(encoding="utf-8").split()
                    if quota != "max":
                        quotas.append(int(quota) / int(period))
    quota = min(quotas) if quotas else None
    if quota is not None and quota < len(cores):
        raise ValueError(f"{len(cores)} server/client cores require more capacity than the {quota:g}-CPU quota")
    return {"server_cores": len(server), "client_cores": len(client), "cpu_quota": quota,
            "available_cpus": sorted(affinity)}


def read_topology(startup, expected):
    candidates = [line.removeprefix(TOPOLOGY_MARKER) for line in startup.splitlines()
                  if line.startswith(TOPOLOGY_MARKER)]
    if len(candidates) != 1:
        raise ValueError("Expected one effective topology marker from the SUT")
    assignments = candidates[0].split()
    fields = dict(field.split("=", 1) for field in assignments)
    if len(assignments) != len(TOPOLOGY_KEYS) or fields.keys() != TOPOLOGY_KEYS:
        raise ValueError("Unexpected topology fields from the SUT")
    topology = {key: value if key == "ingress" else int(value) for key, value in fields.items()}
    if (topology["http_network_threads"] not in (1, 2)
            or any(topology[key] != value for key, value in expected.items())):
        raise ValueError(f"SUT reported topology {topology}, expected {expected}")
    return topology


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
    fixed_real_task = args.seed_pipeline_task and mode not in FIXTURE_MODES and scenario == GET_TASK
    if args.seed_pipeline_task:
        environment["A2A_PERF_SEED_PIPELINE_TASK"] = str(int(fixed_real_task))
    threads, ingress = args.executors.get(name, (None, "shared"))
    network, application = args.http_worker_counts.get(name, (None, None))
    environment.update(A2A_PERF_DISPATCHER_THREADS=str(threads or 0), A2A_PERF_INGRESS=ingress,
                       A2A_PERF_HTTP_NETWORK_THREADS=str(network or 0),
                       A2A_PERF_HTTP_APPLICATION_THREADS=str(application or 0))
    with log_path.open("w", encoding="utf-8") as log:
        server = subprocess.Popen(pinned(args.server_cpus, str(Path(binary).resolve()), f"{HOST}:{port}"),
                                  env=environment, stdout=log, stderr=subprocess.STDOUT)
    try:
        wait_for_port(HOST, target_port, server, log_path)
        startup = log_path.read_text(encoding="utf-8")
        if mode != "sync" and f"A2A_PERFORMANCE_MODE server={mode} " not in startup:
            raise ValueError(f"{name} did not confirm the requested server mode")
        if mode in ASYNC_MODES and (args.require_topology or threads is not None or name in args.http_worker_counts):
            expected_topology = {"dispatcher_threads": threads or 2, "ingress": ingress,
                                 "http_application_threads": application or 4,
                                 "http_pending_limit": args.http_pending_limit or 128}
            if network is not None:
                expected_topology["http_network_threads"] = network
            coordinates.update(read_topology(startup, expected_topology))
        resources = {}
        command = pinned(args.client_cpus, str(args.driver.resolve()), "--transport", transport,
                         "--store-backend", "inmemory", "--host", HOST, "--port", str(port),
                         "--requests", str(args.requests), "--concurrency", str(concurrency),
                         "--warmup-seconds", str(args.warmup_seconds), "--duration-seconds", str(args.duration_seconds),
                         "--client-mode", client, "--scenarios", scenario)
        if mode in FIXTURE_MODES or fixed_real_task:
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
                mode = variant[1]
                if mode in RAW_FIXTURE_MODES and transport != "http_json":
                    continue
                if mode in FIXTURE_MODES and (scenario != GET_TASK or delay):
                    continue
                if transport not in args.transport_filters.get(variant[0], args.transports):
                    continue
                yield variant, transport, scenario, client, delay, concurrency, repetition


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--server", action="append", nargs=3, metavar=("LABEL", "MODE", "BINARY"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--server-cpus")
    parser.add_argument("--client-cpus")
    parser.add_argument("--require-dedicated-cores", action="store_true")
    parser.add_argument("--executor", action="append", nargs=3, metavar=("LABEL", "THREADS", "INGRESS"), default=[])
    parser.add_argument("--http-workers", action="append", nargs=3, metavar=("LABEL", "NETWORK", "APPLICATION"),
                        default=[], help="Set HTTP worker counts with 'auto' for either default")
    parser.add_argument("--only-transports", action="append", nargs="+", metavar="LABEL_OR_TRANSPORT", default=[])
    parser.add_argument("--require-topology", action="store_true", help="Require effective async SUT settings")
    parser.add_argument("--seed-pipeline-task", action="store_true")
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
    modes = {}
    for name, mode, _ in args.server:
        if not name or Path(name).name != name or name in modes or mode not in {"sync", "async", *FIXTURE_MODES}:
            parser.error("Each server needs a distinct label and mode sync, async, listener, immediate, "
                         "pipeline-sync or pipeline-async")
        modes[name] = mode
    args.executors = {}
    for name, threads, ingress in args.executor:
        if (modes.get(name) not in ASYNC_MODES or name in args.executors or not threads.isascii()
                or not threads.isdecimal() or not 1 <= int(threads) <= 64 or ingress not in {"shared", "separate"}):
            parser.error("Each executor needs a distinct async server label, 1–64 threads, and shared or separate ingress")
        args.executors[name] = (int(threads), ingress)
    args.http_worker_counts = {}
    for name, network, application in args.http_workers:
        if (modes.get(name) not in ASYNC_MODES or name in args.http_worker_counts
                or network not in {"auto", "1", "2"}
                or application not in {"auto", *(str(number) for number in range(1, 65))}):
            parser.error("Each HTTP worker setting needs a distinct async server label, network count auto/1/2, "
                         "and application count auto/1–64")
        args.http_worker_counts[name] = tuple(None if count == "auto" else int(count) for count in (network, application))
    args.transport_filters = {}
    for name, *transports in args.only_transports:
        if name not in modes or name in args.transport_filters or not transports or not set(transports) <= set(args.transports):
            parser.error("Each transport filter needs a distinct server label and transports in the selected matrix")
        args.transport_filters[name] = transports
    if args.require_dedicated_cores:
        if not args.server_cpus or not args.client_cpus:
            parser.error("Dedicated cores require both server and client CPU lists")
        try:
            args.allocation = cpu_allocation(args.server_cpus, args.client_cpus)
        except (ValueError, OSError, AttributeError) as error:
            parser.error(str(error))
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
