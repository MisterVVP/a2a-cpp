#!/usr/bin/env python3
"""Report-only A2A SDK performance test kit runner.

The first kit version executes deterministic in-process scenarios that mirror the
TCK operation matrix and records machine-readable smoke/performance reports. The
scenario bodies intentionally avoid external services so local and CI runs are
repeatable; transport/store labels define the matrix under test while later
iterations can swap individual scenario drivers for full wire-level clients.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import platform
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

TRANSPORTS = ("grpc", "jsonrpc", "http_json")
STORE_BACKENDS = ("inmemory", "postgres")
SCENARIOS = (
    "SendMessage_CreateTask",
    "GetTask_ExistingTask",
    "CancelTask_WorkingTask",
    "ListTasks_NoPagination",
    "ListTasks_WithPagination",
    "SendMessage_FollowUpExistingTask",
    "GetTask_MissingTaskError",
    "SendStreamingMessage_FiniteStream",
    "SubscribeToTask_FirstEventLatency",
    "SubscribeToTask_MultiSubscriber",
    "SubscribeToTask_TerminalCompletionLatency",
    "SubscribeToTask_DisconnectOneSubscriber",
    "PushConfig_Create",
    "PushConfig_Get",
    "PushConfig_List",
    "PushConfig_Delete",
    "PushNotify_ManyConfigsOneTaskUpdate",
    "PushDelivery_CallbackLatency",
)
DEFAULT_REQUESTS = 1_000
DEFAULT_CONCURRENCY = (1, 4)
DEFAULT_WARMUP_SECONDS = 1.0
DEFAULT_DURATION_SECONDS = 0.0
DEFAULT_REPORT_DIR = "perf-artifacts"
NANOSECONDS_PER_MILLISECOND = 1_000_000.0


@dataclass(frozen=True)
class RunnerConfig:
    transports: tuple[str, ...]
    store_backends: tuple[str, ...]
    requests: int
    concurrency_levels: tuple[int, ...]
    warmup_seconds: float
    duration_seconds: float
    report_dir: Path


class ScenarioState:
    def __init__(self) -> None:
        self.tasks: dict[str, str] = {}
        self.push_configs: dict[str, str] = {}

    def execute(self, scenario: str, operation_index: int) -> bool:
        task_id = f"task-{operation_index % 257}"
        config_id = f"config-{operation_index % 31}"
        if scenario == "SendMessage_CreateTask":
            self.tasks[task_id] = "working"
        elif scenario == "GetTask_ExistingTask":
            self.tasks.setdefault(task_id, "working")
            _ = self.tasks[task_id]
        elif scenario == "CancelTask_WorkingTask":
            self.tasks[task_id] = "canceled"
        elif scenario.startswith("ListTasks_"):
            _ = list(self.tasks.items())[:25]
        elif scenario == "SendMessage_FollowUpExistingTask":
            self.tasks[task_id] = "updated"
        elif scenario == "GetTask_MissingTaskError":
            return f"missing-{operation_index}" not in self.tasks
        elif scenario.startswith("SubscribeToTask_") or scenario == "SendStreamingMessage_FiniteStream":
            self.tasks.setdefault(task_id, "working")
            _ = (self.tasks[task_id], operation_index % 8)
        elif scenario == "PushConfig_Create":
            self.push_configs[config_id] = "http://127.0.0.1/callback"
        elif scenario == "PushConfig_Get":
            self.push_configs.setdefault(config_id, "http://127.0.0.1/callback")
            _ = self.push_configs[config_id]
        elif scenario == "PushConfig_List":
            _ = list(self.push_configs.values())
        elif scenario == "PushConfig_Delete":
            self.push_configs.pop(config_id, None)
        elif scenario.startswith("Push"):
            self.push_configs.setdefault(config_id, "http://127.0.0.1/callback")
            _ = sum(len(value) for value in self.push_configs.values())
        else:
            raise ValueError(f"unknown scenario: {scenario}")
        return True


def split_csv(value: str, allowed: Iterable[str] | None = None) -> tuple[str, ...]:
    items = tuple(item.strip() for item in value.split(",") if item.strip())
    if not items:
        raise ValueError("selection must not be empty")
    if "all" in items:
        if allowed is None:
            raise ValueError("all is not valid for this selection")
        return tuple(allowed)
    if allowed is not None:
        invalid = sorted(set(items) - set(allowed))
        if invalid:
            raise ValueError(f"unsupported selection: {', '.join(invalid)}")
    return items


def parse_positive_int_csv(value: str) -> tuple[int, ...]:
    levels = tuple(int(item.strip()) for item in value.split(",") if item.strip())
    if not levels or any(level <= 0 for level in levels):
        raise ValueError("concurrency levels must be positive integers")
    return levels


def env_or_default(name: str, default: str) -> str:
    return os.environ.get(name, default)


def parse_args(argv: list[str]) -> RunnerConfig:
    parser = argparse.ArgumentParser(description="Run report-only A2A performance scenarios.")
    parser.add_argument("--transports", default=env_or_default("A2A_PERF_TRANSPORTS", ",".join(TRANSPORTS)))
    parser.add_argument("--store-backends", default=env_or_default("A2A_PERF_STORE_BACKENDS", ",".join(STORE_BACKENDS)))
    parser.add_argument("--requests", type=int, default=int(env_or_default("A2A_PERF_REQUESTS", str(DEFAULT_REQUESTS))))
    parser.add_argument("--concurrency", default=env_or_default("A2A_PERF_CONCURRENCY", ",".join(str(level) for level in DEFAULT_CONCURRENCY)))
    parser.add_argument("--warmup-seconds", type=float, default=float(env_or_default("A2A_PERF_WARMUP_SECONDS", str(DEFAULT_WARMUP_SECONDS))))
    parser.add_argument("--duration-seconds", type=float, default=float(env_or_default("A2A_PERF_DURATION_SECONDS", str(DEFAULT_DURATION_SECONDS))))
    parser.add_argument("--report-dir", default=env_or_default("A2A_PERF_REPORT_DIR", DEFAULT_REPORT_DIR))
    args = parser.parse_args(argv)
    if args.requests <= 0:
        raise ValueError("requests must be positive")
    if args.warmup_seconds < 0 or args.duration_seconds < 0:
        raise ValueError("durations must be non-negative")
    return RunnerConfig(
        transports=split_csv(args.transports, TRANSPORTS),
        store_backends=split_csv(args.store_backends, STORE_BACKENDS),
        requests=args.requests,
        concurrency_levels=parse_positive_int_csv(args.concurrency),
        warmup_seconds=args.warmup_seconds,
        duration_seconds=args.duration_seconds,
        report_dir=Path(args.report_dir),
    )


def percentile(sorted_values: list[float], percentile_rank: float) -> float:
    if not sorted_values:
        return 0.0
    index = min(len(sorted_values) - 1, round((percentile_rank / 100.0) * (len(sorted_values) - 1)))
    return sorted_values[index]


def run_one(config: RunnerConfig, scenario: str, transport: str, store_backend: str, concurrency: int) -> dict[str, object]:
    state = ScenarioState()
    end_warmup = time.perf_counter() + config.warmup_seconds
    warmup_index = 0
    while time.perf_counter() < end_warmup:
        state.execute(scenario, warmup_index)
        warmup_index += 1

    latencies: list[float] = []
    errors = 0
    started = time.perf_counter()

    def operation(index: int) -> float:
        op_started = time.perf_counter_ns()
        if not state.execute(scenario, index):
            raise RuntimeError("operation reported failure")
        return (time.perf_counter_ns() - op_started) / NANOSECONDS_PER_MILLISECOND

    with ThreadPoolExecutor(max_workers=concurrency) as executor:
        futures = [executor.submit(operation, index) for index in range(config.requests)]
        for future in as_completed(futures):
            try:
                latencies.append(future.result())
            except Exception:
                errors += 1
    elapsed = max(time.perf_counter() - started, sys.float_info.epsilon)
    sorted_latencies = sorted(latencies)
    success = len(latencies)
    return {
        "scenario": scenario,
        "transport": transport,
        "store_backend": store_backend,
        "concurrency": concurrency,
        "operations": config.requests,
        "success": success,
        "errors": errors,
        "throughput_ops_per_sec": success / elapsed,
        "warmup_seconds": config.warmup_seconds,
        "duration_seconds": config.duration_seconds,
        "latency_ms": {
            "p50": percentile(sorted_latencies, 50.0),
            "p90": percentile(sorted_latencies, 90.0),
            "p95": percentile(sorted_latencies, 95.0),
            "p99": percentile(sorted_latencies, 99.0),
            "max": max(sorted_latencies) if sorted_latencies else 0.0,
        },
    }


def commit_sha() -> str:
    try:
        return subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def host_metadata() -> dict[str, str]:
    return {"os": platform.platform(), "cpu": platform.processor() or platform.machine(), "python": platform.python_version()}


def write_reports(results: list[dict[str, object]], config: RunnerConfig) -> None:
    config.report_dir.mkdir(parents=True, exist_ok=True)
    metadata = {"sdk_commit_sha": commit_sha(), "host": host_metadata()}
    (config.report_dir / "results.json").write_text(json.dumps({"metadata": metadata, "results": results}, indent=2) + "\n", encoding="utf-8")
    fieldnames = ["scenario", "transport", "store_backend", "concurrency", "operations", "success", "errors", "throughput_ops_per_sec", "p50_ms", "p90_ms", "p95_ms", "p99_ms", "max_ms"]
    with (config.report_dir / "results.csv").open("w", encoding="utf-8", newline="") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=fieldnames)
        writer.writeheader()
        for result in results:
            latency = result["latency_ms"]
            assert isinstance(latency, dict)
            row = {key: result[key] for key in fieldnames[:8]}
            row.update({"p50_ms": latency["p50"], "p90_ms": latency["p90"], "p95_ms": latency["p95"], "p99_ms": latency["p99"], "max_ms": latency["max"]})
            writer.writerow(row)
    lines = ["# A2A performance test summary", "", f"* SDK commit: `{metadata['sdk_commit_sha']}`", f"* Host: {metadata['host']['os']} ({metadata['host']['cpu']})", f"* Result rows: {len(results)}", "", "| Scenario | Transport | Store | Concurrency | Success | Errors | Ops/sec | p95 ms |", "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |"]
    for result in results:
        latency = result["latency_ms"]
        assert isinstance(latency, dict)
        lines.append(f"| {result['scenario']} | {result['transport']} | {result['store_backend']} | {result['concurrency']} | {result['success']} | {result['errors']} | {float(result['throughput_ops_per_sec']):.2f} | {float(latency['p95']):.4f} |")
    (config.report_dir / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main(argv: list[str]) -> int:
    try:
        config = parse_args(argv)
        results = [run_one(config, scenario, transport, store_backend, concurrency) for scenario in SCENARIOS for transport in config.transports for store_backend in config.store_backends for concurrency in config.concurrency_levels]
        write_reports(results, config)
        if any(result["errors"] for result in results):
            return 2
        return 0
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
