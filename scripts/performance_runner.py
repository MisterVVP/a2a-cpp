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
from typing import Callable, Iterable

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
WIRE_DRIVER_NAME = "a2a_wire_performance_driver"
SUT_NAME = "tck_sut"
DEFAULT_WARMUP_SECONDS = 1.0
DEFAULT_DURATION_SECONDS = 0.0
DEFAULT_REPORT_DIR = "perf-artifacts"
WIRE_SCENARIOS = (
    "ListTasks_NoPagination",
    "ListTasks_WithPagination",
    "SendMessage_CreateTask",
    "GetTask_ExistingTask",
    "CancelTask_WorkingTask",
    "SendMessage_FollowUpExistingTask",
    "GetTask_MissingTaskError",
    "SendStreamingMessage_FiniteStream",
    "SubscribeToTask_FirstEventLatency",
    "PushConfig_Create",
    "PushConfig_Get",
    "PushConfig_List",
    "PushConfig_Delete",
)
WIRE_TRANSPORT_PATHS = {"http_json": "wire_http_json", "jsonrpc": "wire_jsonrpc", "grpc": "wire_grpc"}
WIRE_PUSH_CONFIG_SCENARIOS = {"PushConfig_Create", "PushConfig_Get", "PushConfig_List", "PushConfig_Delete"}
SUT_READY_TIMEOUT_SECONDS = 30.0
DEFAULT_DRIVER_TIMEOUT_SECONDS = 600.0
DEFAULT_WIRE_DRIVER_TIMEOUT_SECONDS = 600.0
MAX_ERROR_ROWS_TO_PRINT = 20


@dataclass(frozen=True)
class RunnerConfig:
    transports: tuple[str, ...]
    store_backends: tuple[str, ...]
    requests: int
    concurrency_levels: tuple[int, ...]
    warmup_seconds: float
    duration_seconds: float
    report_dir: Path
    driver_timeout_seconds: float
    wire_driver_timeout_seconds: float



def driver_path_from_build(build_dir: Path) -> Path:
    candidates = [build_dir / "tests" / DRIVER_NAME, build_dir / DRIVER_NAME]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return candidates[0]


def executable_path_from_build(build_dir: Path, name: str) -> Path:
    candidates = [build_dir / "tests" / name, build_dir / name]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return candidates[0]


def ensure_executable(config: RunnerConfig, env_name: str, target_name: str, description: str) -> Path:
    explicit = os.environ.get(env_name)
    if explicit:
        executable = Path(explicit)
        if not executable.exists():
            raise ValueError(f"{env_name} does not exist: {executable}")
        return executable
    build_dir = Path(os.environ.get("A2A_PERF_BUILD_DIR", DEFAULT_BUILD_DIR))
    executable = executable_path_from_build(build_dir, target_name)
    if executable.exists():
        return executable
    configure = ["cmake", "-S", ".", "-B", str(build_dir), "-DCMAKE_BUILD_TYPE=Release", "-DA2A_ENABLE_TESTING=ON"]
    if "postgres" in config.store_backends:
        configure.append("-DA2A_ENABLE_POSTGRES_STORE=ON")
    subprocess.run(configure, check=True)
    subprocess.run(["cmake", "--build", str(build_dir), "--target", target_name, "-j", str(os.cpu_count() or 2)], check=True)
    if not executable.exists():
        raise ValueError(f"{description} was not produced at {executable}")
    return executable


def ensure_driver(config: RunnerConfig) -> Path:
    return ensure_executable(config, "A2A_PERF_DRIVER", DRIVER_NAME, "performance driver")


def ensure_wire_driver(config: RunnerConfig) -> Path:
    return ensure_executable(config, "A2A_PERF_WIRE_DRIVER", WIRE_DRIVER_NAME, "wire performance driver")



def ensure_sut(config: RunnerConfig) -> Path:
    return ensure_executable(config, "A2A_TCK_SUT", SUT_NAME, "TCK SUT")

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


def find_available_sut_port(host: str = "127.0.0.1") -> int:
    for _ in range(100):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as http_probe:
            http_probe.bind((host, 0))
            port = int(http_probe.getsockname()[1])
            if port >= 65534:
                continue
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as grpc_probe:
                try:
                    grpc_probe.bind((host, port + 1))
                except OSError:
                    continue
            return port
    raise ValueError("could not find adjacent free ports for tck_sut")

def read_tail(path: Path) -> str:
    if not path.exists():
        return "<no log file>"
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    return "\n".join(lines[-80:])


def postgres_schema_name(transport: str, concurrency: int, port: int) -> str:
    safe_transport = "".join(ch if ch.isalnum() else "_" for ch in transport.lower())
    return f"a2a_perf_{safe_transport}_{concurrency}_{port}"


class SutProcess:
    def __init__(self, config: RunnerConfig, store_backend: str, port: int, transport: str, concurrency: int) -> None:
        self.host = "127.0.0.1"
        self.port = port
        self.grpc_port = port + 1
        self.log_path = config.report_dir / f"tck_sut_{store_backend}_{port}.log"
        self.process: subprocess.Popen[str] | None = None
        self.sut = ensure_sut(config)
        self.store_backend = store_backend
        self.transport = transport
        self.concurrency = concurrency

    def __enter__(self) -> "SutProcess":
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        env = os.environ.copy()
        env["A2A_TCK_STORE_BACKEND"] = self.store_backend
        if self.store_backend == "postgres":
            if "A2A_TCK_POSTGRES_DSN" not in env and "A2A_TEST_POSTGRES_DSN" in env:
                env["A2A_TCK_POSTGRES_DSN"] = env["A2A_TEST_POSTGRES_DSN"]
            env["A2A_TCK_POSTGRES_SCHEMA"] = postgres_schema_name(self.transport, self.concurrency, self.port)
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


def run_command_json(command: list[str], timeout_seconds: float, error_context: str, log_path: Path | None = None) -> list[dict[str, object]]:
    process = subprocess.Popen(
        command,
        cwd=Path(__file__).resolve().parents[1],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        stdout, stderr = process.communicate(timeout=timeout_seconds)
    except subprocess.TimeoutExpired as exc:
        process.terminate()
        try:
            stdout, stderr = process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            stdout, stderr = process.communicate()
        message = f"{error_context} timed out after {timeout_seconds:.0f}s"
        if stderr.strip():
            message += f"; stderr: {stderr.strip()}"
        if log_path is not None:
            message += f"; tck_sut logs:\n{read_tail(log_path)}"
        raise ValueError(message) from exc
    if process.returncode != 0:
        raise ValueError(f"{error_context} failed: {stderr.strip()}")
    payload = json.loads(stdout)
    if not isinstance(payload, list):
        raise ValueError(f"{error_context} returned malformed output")
    return payload


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
    return run_command_json(
        command, config.driver_timeout_seconds, f"performance driver for {transport}/{store_backend}/c{concurrency}"
    )



def run_wire_driver(config: RunnerConfig, transport: str, store_backend: str, concurrency: int, port: int) -> list[dict[str, object]]:
    if transport not in WIRE_TRANSPORT_PATHS:
        raise ValueError(f"unsupported wire transport: {transport}")
    wire_driver = ensure_wire_driver(config)
    with SutProcess(config, store_backend, port, transport, concurrency) as sut:
        command = [
            str(wire_driver),
            "--transport", transport,
            "--store-backend", store_backend,
            "--host", sut.host,
            "--port", str(sut.port),
            "--requests", str(config.requests),
            "--concurrency", str(concurrency),
            "--warmup-seconds", str(config.warmup_seconds),
            "--duration-seconds", str(config.duration_seconds),
            "--scenarios", ",".join(wire_scenarios_for_transport(transport)),
        ]
        payload = run_command_json(
            command, config.wire_driver_timeout_seconds,
            f"wire performance driver for {transport}/{store_backend}/c{concurrency}", sut.log_path,
        )
    for result in payload:
        if result.get("driver_type") != "wire_tck_sut" or result.get("transport_path") != WIRE_TRANSPORT_PATHS[transport]:
            raise ValueError("wire performance driver returned misleading metadata")
    return payload


def wire_scenarios_for_transport(transport: str) -> tuple[str, ...]:
    if transport == "grpc":
        return WIRE_SCENARIOS
    return tuple(scenario for scenario in WIRE_SCENARIOS if scenario not in WIRE_PUSH_CONFIG_SCENARIOS)


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
    parser.add_argument("--driver-timeout-seconds", type=float, default=float(env_or_default("A2A_PERF_DRIVER_TIMEOUT_SECONDS", str(DEFAULT_DRIVER_TIMEOUT_SECONDS))))
    parser.add_argument("--wire-driver-timeout-seconds", type=float, default=float(env_or_default("A2A_PERF_WIRE_DRIVER_TIMEOUT_SECONDS", str(DEFAULT_WIRE_DRIVER_TIMEOUT_SECONDS))))
    args = parser.parse_args(argv)
    if args.requests <= 0:
        raise ValueError("requests must be positive")
    if args.warmup_seconds < 0 or args.duration_seconds < 0:
        raise ValueError("durations must be non-negative")
    if args.driver_timeout_seconds <= 0 or args.wire_driver_timeout_seconds <= 0:
        raise ValueError("driver timeouts must be positive")
    return RunnerConfig(
        transports=split_csv(args.transports, TRANSPORTS),
        store_backends=split_csv(args.store_backends, STORE_BACKENDS),
        requests=args.requests,
        concurrency_levels=parse_positive_int_csv(args.concurrency),
        warmup_seconds=args.warmup_seconds,
        duration_seconds=args.duration_seconds,
        report_dir=Path(args.report_dir),
        driver_timeout_seconds=args.driver_timeout_seconds,
        wire_driver_timeout_seconds=args.wire_driver_timeout_seconds,
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
    fieldnames = ["scenario", "transport", "store_backend", "driver_type", "transport_path", "concurrency", "operations", "success", "errors", "throughput_ops_per_sec", "configured_requests", "configured_duration_seconds", "measured_duration_seconds", "successful_deliveries", "failed_deliveries", "callback_count", "event_count", "p50_ms", "p90_ms", "p95_ms", "p99_ms", "max_ms"]
    with csv_path.open("w", encoding="utf-8", newline="") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=fieldnames)
        writer.writeheader()
        for result in results:
            latency = result["latency_ms"]
            assert isinstance(latency, dict)
            row = {key: result.get(key, 0) for key in fieldnames[:17]}
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


def log_progress(message: str) -> None:
    print(f"[perf] {message}", flush=True)


def log_workload_estimate(config: RunnerConfig) -> None:
    store_concurrency_rows = len(config.store_backends) * len(config.concurrency_levels)
    in_process_rows = store_concurrency_rows * len(SCENARIOS)
    wire_rows = sum(len(wire_scenarios_for_transport(transport)) for transport in config.transports) * store_concurrency_rows
    estimated_rows = in_process_rows + wire_rows
    estimated_operations = estimated_rows * config.requests
    log_progress(
        f"estimated_rows={estimated_rows} estimated_operations={estimated_operations} "
        f"transports={len(config.transports)} stores={len(config.store_backends)} "
        f"concurrency_levels={len(config.concurrency_levels)} requests={config.requests}"
    )


def result_error_count(result: dict[str, object]) -> int:
    return int(result.get("errors", 0))


def format_error_summary(results: list[dict[str, object]]) -> str:
    error_rows = [result for result in results if result_error_count(result) > 0]
    total_errors = sum(result_error_count(result) for result in error_rows)
    lines = [f"performance scenarios reported {total_errors} operation errors across {len(error_rows)} rows"]
    for result in error_rows[:MAX_ERROR_ROWS_TO_PRINT]:
        lines.append(
            "- "
            f"scenario={result.get('scenario')} "
            f"driver_type={result.get('driver_type')} "
            f"transport_path={result.get('transport_path')} "
            f"transport={result.get('transport')} "
            f"store={result.get('store_backend')} "
            f"concurrency={result.get('concurrency')} "
            f"success={result.get('success')} "
            f"errors={result.get('errors')}"
        )
    if len(error_rows) > MAX_ERROR_ROWS_TO_PRINT:
        lines.append(f"- ... {len(error_rows) - MAX_ERROR_ROWS_TO_PRINT} additional error rows omitted")
    return "\n".join(lines)


def run_with_progress(label: str, runner: Callable[[], list[dict[str, object]]], transport: str, store_backend: str, concurrency: int, requests: int) -> list[dict[str, object]]:
    log_progress(f"start {label} transport={transport} store={store_backend} concurrency={concurrency} requests={requests}")
    started = time.monotonic()
    results = runner()
    elapsed = time.monotonic() - started
    errors = sum(result_error_count(result) for result in results)
    log_progress(
        f"done  {label} transport={transport} store={store_backend} concurrency={concurrency} "
        f"rows={len(results)} errors={errors} elapsed={elapsed:.2f}s"
    )
    return results


def main(argv: list[str]) -> int:
    try:
        config = parse_args(argv)
        results = []
        log_workload_estimate(config)
        in_process_transport = config.transports[0]
        for store_backend in config.store_backends:
            for concurrency in config.concurrency_levels:
                results.extend(
                    run_with_progress(
                        "in-process",
                        lambda: run_driver(config, in_process_transport, store_backend, concurrency),
                        in_process_transport, store_backend, concurrency, config.requests,
                    )
                )
        for transport in config.transports:
            for store_backend in config.store_backends:
                for concurrency in config.concurrency_levels:
                    results.extend(
                        run_with_progress(
                            "wire",
                            lambda: run_wire_driver(
                                config, transport, store_backend, concurrency, find_available_sut_port()
                            ),
                            transport, store_backend, concurrency, config.requests,
                        )
                    )
        results.sort(key=lambda result: (str(result["scenario"]), str(result["store_backend"]), str(result["driver_type"]), str(result["transport_path"]), str(result["transport"]), int(result["concurrency"])))
        write_reports(results, config)
        if any(result_error_count(result) > 0 for result in results):
            print(f"error: {format_error_summary(results)}", file=sys.stderr)
            return 2
        return 0
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
