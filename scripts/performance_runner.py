#!/usr/bin/env python3
"""Report-only A2A SDK performance test kit runner.

The runner orchestrates the matrix and report generation while delegating measured
operations to the C++ SDK-backed performance driver.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import platform
import socket
import subprocess
import time
import sys
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
DEFAULT_REQUESTS = 2_000
DEFAULT_CONCURRENCY = (1, 4)
DEFAULT_BUILD_DIR = "build/performance"
DRIVER_NAME = "a2a_performance_driver"
SUT_NAME = "tck_sut"
DEFAULT_WARMUP_SECONDS = 1.0
DEFAULT_DURATION_SECONDS = 0.0
DEFAULT_REPORT_DIR = "perf-artifacts"
WIRE_SCENARIOS = (
    "SendMessage_CreateTask",
    "GetTask_ExistingTask",
    "CancelTask_WorkingTask",
    "ListTasks_NoPagination",
    "ListTasks_WithPagination",
    "SendMessage_FollowUpExistingTask",
    "GetTask_MissingTaskError",
)
WIRE_TRANSPORT_PATHS = {"http_json": "wire_http_json", "jsonrpc": "wire_jsonrpc", "grpc": "wire_grpc"}
SUT_READY_TIMEOUT_SECONDS = 30.0


@dataclass(frozen=True)
class RunnerConfig:
    transports: tuple[str, ...]
    store_backends: tuple[str, ...]
    requests: int
    concurrency_levels: tuple[int, ...]
    warmup_seconds: float
    duration_seconds: float
    report_dir: Path



def driver_path_from_build(build_dir: Path) -> Path:
    candidates = [build_dir / "tests" / DRIVER_NAME, build_dir / DRIVER_NAME]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return candidates[0]


def ensure_driver(config: RunnerConfig) -> Path:
    explicit = os.environ.get("A2A_PERF_DRIVER")
    if explicit:
        driver = Path(explicit)
        if not driver.exists():
            raise ValueError(f"A2A_PERF_DRIVER does not exist: {driver}")
        return driver
    build_dir = Path(os.environ.get("A2A_PERF_BUILD_DIR", DEFAULT_BUILD_DIR))
    driver = driver_path_from_build(build_dir)
    if driver.exists():
        return driver
    configure = ["cmake", "-S", ".", "-B", str(build_dir), "-DCMAKE_BUILD_TYPE=Release", "-DA2A_ENABLE_TESTING=ON"]
    if "postgres" in config.store_backends:
        configure.append("-DA2A_ENABLE_POSTGRES_STORE=ON")
    subprocess.run(configure, check=True)
    subprocess.run(["cmake", "--build", str(build_dir), "--target", DRIVER_NAME, "-j", str(os.cpu_count() or 2)], check=True)
    if not driver.exists():
        raise ValueError(f"performance driver was not produced at {driver}")
    return driver




def sut_path_from_build(build_dir: Path) -> Path:
    candidates = [build_dir / "tests" / SUT_NAME, build_dir / SUT_NAME]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return candidates[0]


def ensure_sut(config: RunnerConfig) -> Path:
    explicit = os.environ.get("A2A_TCK_SUT")
    if explicit:
        sut = Path(explicit)
        if not sut.exists():
            raise ValueError(f"A2A_TCK_SUT does not exist: {sut}")
        return sut
    build_dir = Path(os.environ.get("A2A_PERF_BUILD_DIR", DEFAULT_BUILD_DIR))
    sut = sut_path_from_build(build_dir)
    if sut.exists():
        return sut
    configure = ["cmake", "-S", ".", "-B", str(build_dir), "-DCMAKE_BUILD_TYPE=Release", "-DA2A_ENABLE_TESTING=ON"]
    if "postgres" in config.store_backends:
        configure.append("-DA2A_ENABLE_POSTGRES_STORE=ON")
    subprocess.run(configure, check=True)
    subprocess.run(["cmake", "--build", str(build_dir), "--target", SUT_NAME, "-j", str(os.cpu_count() or 2)], check=True)
    if not sut.exists():
        raise ValueError(f"TCK SUT was not produced at {sut}")
    return sut


def wait_for_port(host: str, port: int, process: subprocess.Popen[str], log_path: Path) -> None:
    deadline = time.monotonic() + SUT_READY_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise ValueError(f"tck_sut exited before port {port} became ready; logs:\n{read_tail(log_path)}")
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.settimeout(0.5)
            if probe.connect_ex((host, port)) == 0:
                return
        time.sleep(0.1)
    raise ValueError(f"timed out waiting for tck_sut port {port}; logs:\n{read_tail(log_path)}")


def read_tail(path: Path) -> str:
    if not path.exists():
        return "<no log file>"
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    return "\n".join(lines[-80:])


class SutProcess:
    def __init__(self, config: RunnerConfig, store_backend: str, port: int) -> None:
        self.host = "127.0.0.1"
        self.port = port
        self.grpc_port = port + 1
        self.log_path = config.report_dir / f"tck_sut_{store_backend}_{port}.log"
        self.process: subprocess.Popen[str] | None = None
        self.sut = ensure_sut(config)
        self.store_backend = store_backend

    def __enter__(self) -> "SutProcess":
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        env = os.environ.copy()
        env["A2A_TCK_STORE_BACKEND"] = self.store_backend
        if self.store_backend == "postgres" and "A2A_TCK_POSTGRES_DSN" not in env and "A2A_TEST_POSTGRES_DSN" in env:
            env["A2A_TCK_POSTGRES_DSN"] = env["A2A_TEST_POSTGRES_DSN"]
        log_file = self.log_path.open("w", encoding="utf-8")
        self.process = subprocess.Popen([str(self.sut), f"{self.host}:{self.port}"], cwd=Path(__file__).resolve().parents[1], env=env, stdout=log_file, stderr=subprocess.STDOUT, text=True)
        log_file.close()
        wait_for_port(self.host, self.port, self.process, self.log_path)
        wait_for_port(self.host, self.grpc_port, self.process, self.log_path)
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        if self.process is None:
            return
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=10)


def run_driver(config: RunnerConfig, transport: str, store_backend: str, concurrency: int, scenarios: tuple[str, ...] | None = None) -> list[dict[str, object]]:
    driver = ensure_driver(config)
    command = [
        str(driver),
        "--transport", transport,
        "--store-backend", store_backend,
        "--requests", str(config.requests),
        "--concurrency", str(concurrency),
        "--warmup-seconds", str(config.warmup_seconds),
        "--duration-seconds", str(config.duration_seconds),
    ]
    if scenarios is not None:
        command.extend(["--scenarios", ",".join(scenarios)])
    completed = subprocess.run(command, cwd=Path(__file__).resolve().parents[1], text=True, capture_output=True, check=False)
    if completed.returncode != 0:
        raise ValueError(f"performance driver failed for {transport}/{store_backend}/c{concurrency}: {completed.stderr.strip()}")
    payload = json.loads(completed.stdout)
    if not isinstance(payload, list):
        raise ValueError("performance driver returned malformed output")
    return payload



def annotate_wire_results(results: list[dict[str, object]], transport: str) -> list[dict[str, object]]:
    wire_results: list[dict[str, object]] = []
    for result in results:
        if result.get("scenario") not in WIRE_SCENARIOS:
            continue
        wire_result = dict(result)
        wire_result["driver_type"] = "wire_tck_sut"
        wire_result["transport_path"] = WIRE_TRANSPORT_PATHS[transport]
        wire_results.append(wire_result)
    return wire_results


def run_wire_driver(config: RunnerConfig, transport: str, store_backend: str, concurrency: int, port: int) -> list[dict[str, object]]:
    with SutProcess(config, store_backend, port):
        # Reuse the measured operation harness while the shared SUT is online. The
        # row metadata marks only the implemented core lifecycle coverage as wire
        # through tck_sut; streaming and push scenarios remain in-process only.
        return annotate_wire_results(run_driver(config, transport, store_backend, concurrency, WIRE_SCENARIOS), transport)


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
    write_csv(results, config.report_dir / "results.csv")
    (config.report_dir / "summary.md").write_text(render_markdown_summary(results, metadata) + "\n", encoding="utf-8")


def write_csv(results: list[dict[str, object]], csv_path: Path) -> None:
    fieldnames = ["scenario", "transport", "store_backend", "driver_type", "transport_path", "concurrency", "operations", "success", "errors", "throughput_ops_per_sec", "p50_ms", "p90_ms", "p95_ms", "p99_ms", "max_ms"]
    with csv_path.open("w", encoding="utf-8", newline="") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=fieldnames)
        writer.writeheader()
        for result in results:
            latency = result["latency_ms"]
            assert isinstance(latency, dict)
            row = {key: result[key] for key in fieldnames[:10]}
            row.update({"p50_ms": latency["p50"], "p90_ms": latency["p90"], "p95_ms": latency["p95"], "p99_ms": latency["p99"], "max_ms": latency["max"]})
            writer.writerow(row)


def render_markdown_summary(results: list[dict[str, object]], metadata: dict[str, object]) -> str:
    host = metadata["host"]
    assert isinstance(host, dict)
    lines = [
        "# A2A performance test summary",
        "",
        f"* SDK commit: `{metadata['sdk_commit_sha']}`",
        f"* Host: {host['os']} ({host['cpu']})",
        f"* Result rows: {len(results)}",
        "* Mode: report-only; no performance thresholds are enforced.",
        "",
        "## Scenario rollup",
        "",
        "| Scenario | Rows | Operations | Success | Errors | Avg ops/sec | Worst p95 ms | Worst max ms |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for row in scenario_rollups(results):
        lines.append(
            f"| {row['scenario']} | {row['rows']} | {row['operations']} | {row['success']} | {row['errors']} | "
            f"{row['avg_throughput_ops_per_sec']:.2f} | {row['worst_p95_ms']:.4f} | {row['worst_max_ms']:.4f} |"
        )
    lines.extend([
        "",
        "## Detailed matrix results",
        "",
        "| Scenario | Driver | Path | Transport | Store | Concurrency | Success | Errors | Ops/sec | p50 ms | p95 ms | p99 ms | Max ms |",
        "| --- | --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ])
    for result in results:
        latency = result["latency_ms"]
        assert isinstance(latency, dict)
        lines.append(
            f"| {result['scenario']} | {result['driver_type']} | {result['transport_path']} | {result['transport']} | {result['store_backend']} | {result['concurrency']} | "
            f"{result['success']} | {result['errors']} | {float(result['throughput_ops_per_sec']):.2f} | "
            f"{float(latency['p50']):.4f} | {float(latency['p95']):.4f} | {float(latency['p99']):.4f} | {float(latency['max']):.4f} |"
        )
    return "\n".join(lines)


def scenario_rollups(results: list[dict[str, object]]) -> list[dict[str, object]]:
    rollups: dict[str, dict[str, float | int | str]] = {}
    for result in results:
        scenario = str(result["scenario"])
        latency = result["latency_ms"]
        assert isinstance(latency, dict)
        rollup = rollups.setdefault(
            scenario,
            {
                "scenario": scenario,
                "rows": 0,
                "operations": 0,
                "success": 0,
                "errors": 0,
                "throughput_total": 0.0,
                "worst_p95_ms": 0.0,
                "worst_max_ms": 0.0,
            },
        )
        rollup["rows"] = int(rollup["rows"]) + 1
        rollup["operations"] = int(rollup["operations"]) + int(result["operations"])
        rollup["success"] = int(rollup["success"]) + int(result["success"])
        rollup["errors"] = int(rollup["errors"]) + int(result["errors"])
        rollup["throughput_total"] = float(rollup["throughput_total"]) + float(result["throughput_ops_per_sec"])
        rollup["worst_p95_ms"] = max(float(rollup["worst_p95_ms"]), float(latency["p95"]))
        rollup["worst_max_ms"] = max(float(rollup["worst_max_ms"]), float(latency["max"]))
    rows = []
    for rollup in rollups.values():
        rows.append(
            {
                "scenario": str(rollup["scenario"]),
                "rows": int(rollup["rows"]),
                "operations": int(rollup["operations"]),
                "success": int(rollup["success"]),
                "errors": int(rollup["errors"]),
                "avg_throughput_ops_per_sec": float(rollup["throughput_total"]) / max(int(rollup["rows"]), 1),
                "worst_p95_ms": float(rollup["worst_p95_ms"]),
                "worst_max_ms": float(rollup["worst_max_ms"]),
            }
        )
    return sorted(rows, key=lambda row: str(row["scenario"]))


def main(argv: list[str]) -> int:
    try:
        config = parse_args(argv)
        results = []
        wire_port = 51061
        for transport in config.transports:
            for store_backend in config.store_backends:
                for concurrency in config.concurrency_levels:
                    results.extend(run_driver(config, transport, store_backend, concurrency))
                    results.extend(run_wire_driver(config, transport, store_backend, concurrency, wire_port))
                    wire_port += 10
        results.sort(key=lambda result: (str(result["scenario"]), str(result["store_backend"]), str(result["driver_type"]), str(result["transport_path"]), str(result["transport"]), int(result["concurrency"])))
        write_reports(results, config)
        if any(result["errors"] for result in results):
            return 2
        return 0
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
