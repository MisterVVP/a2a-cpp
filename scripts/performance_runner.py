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
import re
import secrets
import signal
import socket
import statistics
import subprocess
import threading
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
    "SendMessage_FollowUpAtHistoryDepth/8",
    "GetTask_MissingTaskError",
    "SendStreamingMessage_FiniteStream",
    "SubscribeToTask_FirstEventLatency",
    "IdleStream_ClientCancellationLatency",
    "SubscribeToTask_MultiSubscriber",
    "SubscribeToTask_TerminalCompletionLatency",
    "SubscribeToTask_DisconnectOneSubscriber",
    "PushConfig_Create",
    "PushConfig_Get",
    "PushConfig_List",
    "PushConfig_Delete",
    "PushNotify_EndToEndManyConfigs",
    "PushConfig_ListManyConfigs",
    "PushDelivery_CallbackFanout",
    "PushConfig_CreateMany",
    "PushDelivery_BuildPayload",
)
DEFAULT_REQUESTS = 2_000
DEFAULT_CONCURRENCY = (1, 4)
DEFAULT_PUSH_CONFIG_FANOUT = 8
DEFAULT_BUILD_DIR = "build/performance"
SUBSCRIPTION_DIAGNOSTICS_BUILD_SUFFIX = "-subscription-diagnostics"
SUBSCRIPTION_DIAGNOSTICS_CMAKE_CACHE_PREFIX = "A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS:BOOL="
DRIVER_NAME = "a2a_performance_driver"
WIRE_DRIVER_NAME = "a2a_wire_performance_driver"
SUT_NAME = "tck_sut"
DEFAULT_WARMUP_SECONDS = 1.0
DEFAULT_DURATION_SECONDS = 0.0
DEFAULT_REPORT_DIR = "perf-artifacts"
POSTGRES_TAIL_PROFILE = "postgres-tail"
POSTGRES_TAIL_C1_PROFILE = "postgres-tail-c1"
POSTGRES_WRITE_PROFILE = "postgres-write"
POSTGRES_TAIL_PROFILES = (POSTGRES_TAIL_PROFILE, POSTGRES_TAIL_C1_PROFILE)
POSTGRES_PROFILES = (*POSTGRES_TAIL_PROFILES, POSTGRES_WRITE_PROFILE)
POSTGRES_WRITE_SCENARIOS = (
    "SendMessage_CreateTask",
    "SendMessage_FollowUpExistingTask",
    "PushConfig_Create",
    "PushConfig_CreateMany",
)
POSTGRES_WRITE_CONCURRENCY = (1, 4, 16, 64)
POSTGRES_WRITE_POOL_SIZES = (64,)
POSTGRES_WRITE_REPETITIONS = 5
POSTGRES_SAMPLE_INTERVAL_SECONDS = 0.05
POSTGRES_TAIL_SCENARIOS = (
    "PushNotify_EndToEndManyConfigs",
    "PushConfig_CreateMany",
    "PushConfig_ListManyConfigs",
    "SendMessage_FollowUpExistingTask",
    "SendMessage_CreateTask",
)
POSTGRES_TAIL_C1_SCENARIOS = ("PushConfig_ListManyConfigs",)
POSTGRES_TAIL_CONCURRENCY = (4, 16, 64)
POSTGRES_TAIL_C1_CONCURRENCY = (1,)
POSTGRES_TAIL_REPETITIONS = 5
DEFAULT_POSTGRES_POOL_SIZE = 4
POSTGRES_TAIL_POOL_SIZES = (4, 16, 64)
POSTGRES_TAIL_C1_POOL_SIZES = (64,)
WIRE_SCENARIOS = (
    "ListTasks_NoPagination",
    "ListTasks_WithPagination",
    "SendMessage_CreateTask",
    "GetTask_ExistingTask",
    "CancelTask_WorkingTask",
    "SendMessage_FollowUpExistingTask",
    "SendMessage_FollowUpAtHistoryDepth/8",
    "GetTask_MissingTaskError",
    "SendStreamingMessage_FiniteStream",
    "SubscribeToTask_FirstEventLatency",
    "IdleStream_ClientCancellationLatency",
    "PushConfig_Create",
    "PushConfig_Get",
    "PushConfig_List",
    "PushConfig_Delete",
)
WIRE_TRANSPORT_PATHS = {"http_json": "wire_http_json", "jsonrpc": "wire_jsonrpc", "grpc": "wire_grpc"}
IN_PROCESS_SCENARIOS = tuple(scenario for scenario in SCENARIOS if scenario != "IdleStream_ClientCancellationLatency")
SUT_READY_TIMEOUT_SECONDS = 30.0
SUT_GRACEFUL_SHUTDOWN_TIMEOUT_SECONDS = 10.0
SUT_FORCE_KILL_TIMEOUT_SECONDS = 10.0
SUT_PORT_RANGE_START = 20_000
SUT_PORT_RANGE_END = 30_000
SUT_PORT_PAIR_STEP = 2
SUT_PORT_PAIR_COUNT = (SUT_PORT_RANGE_END - SUT_PORT_RANGE_START) // SUT_PORT_PAIR_STEP
DEFAULT_DRIVER_TIMEOUT_SECONDS = 600.0
DEFAULT_WIRE_DRIVER_TIMEOUT_SECONDS = 600.0
MAX_ERROR_ROWS_TO_PRINT = 20
HTTP_DIAGNOSTICS_PATTERN = re.compile(
    r"A2A_HTTP_DIAGNOSTICS accepted_connections=(\d+) completed_unary_operations=(\d+) "
    r"operations_per_connection=([0-9]+(?:\.[0-9]*)?(?:[eE][+-]?[0-9]+)?) "
    r"finite_stream_connections=(\d+) completed_finite_streams=(\d+) "
    r"finite_streams_per_connection=([0-9]+(?:\.[0-9]*)?(?:[eE][+-]?[0-9]+)?) "
    r"connections_reused_after_finite_stream=(\d+)(?=\s|$)"
)
SUBSCRIPTION_DIAGNOSTICS_PREFIX = "A2A_SUBSCRIPTION_SERVER_DIAGNOSTICS"
SUBSCRIPTION_DIAGNOSTIC_PHASES = (
    "server_cancel_task_total",
    "terminal_store_update",
    "terminal_publication_total",
    "subscriber_resume_callback",
    "proto_to_json",
    "frame_construction",
    "http_delivery",
    "client_terminal_observer_callback",
    "client_completion_callback",
)
POSTGRES_DIAGNOSTIC_PHASES = (
    "connection_acquire_wait",
    "task_get",
    "task_upsert",
    "task_history_snapshot",
    "task_history_lock_read",
    "push_config_upsert",
    "push_config_get",
    "push_config_delete",
    "push_config_list_count",
    "push_config_list_select",
    "transaction_begin",
    "transaction_commit",
)
POSTGRES_QUERY_PLAN_TASK_ID = "a2a-query-plan-probe-task"
POSTGRES_QUERY_PLAN_CONFIG_ID = "a2a-query-plan-probe-config"
POSTGRES_QUERY_PLAN_URL = "https://example.invalid/a2a-query-plan"
POSTGRES_COMBINED_PLAN_START = "A2A_COMBINED_PLAN_START"
POSTGRES_COMBINED_PLAN_END = "A2A_COMBINED_PLAN_END"
POSTGRES_PUSH_ORDER_INDEX = "idx_a2a_push_configs_created_sequence"


@dataclass(frozen=True)
class RunnerConfig:
    profile: str | None
    transports: tuple[str, ...]
    store_backends: tuple[str, ...]
    requests: int
    push_config_fanout: int
    concurrency_levels: tuple[int, ...]
    warmup_seconds: float
    duration_seconds: float
    report_dir: Path
    driver_timeout_seconds: float
    wire_driver_timeout_seconds: float
    repetitions: int
    scenarios: tuple[str, ...] | None
    postgres_pool_sizes: tuple[int, ...]


def run_psql_json(dsn: str, sql: str) -> object:
    completed = subprocess.run(
        ["psql", dsn, "--no-psqlrc", "--tuples-only", "--no-align", "--set", "ON_ERROR_STOP=1",
         "--command", f"SELECT json_build_object({sql});"],
        check=True, capture_output=True, text=True,
    )
    output = completed.stdout.strip()
    if not output:
        raise ValueError("PostgreSQL diagnostic query returned no data")
    return json.loads(output)


POSTGRES_COUNTER_SQL = """
'database', (SELECT row_to_json(s) FROM (
  SELECT xact_commit, xact_rollback, blks_read, blks_hit, tup_inserted, tup_updated,
         blk_read_time, blk_write_time
  FROM pg_stat_database WHERE datname = current_database()) s),
'wal', (SELECT row_to_json(s) FROM (
  SELECT wal_records, wal_fpi, wal_bytes::text::numeric, wal_buffers_full,
         wal_write, wal_sync, wal_write_time, wal_sync_time FROM pg_stat_wal) s),
'slru', (SELECT coalesce(json_object_agg(name, row_to_json(s)), '{}'::json) FROM (
  SELECT name, blks_zeroed, blks_hit, blks_read, blks_written, flushes, truncates
  FROM pg_stat_slru WHERE name IN ('MultiXactMember', 'MultiXactOffset')) s)
"""

POSTGRES_ACTIVITY_SQL = """
'activity', (SELECT json_build_object(
  'sessions', count(*) FILTER (WHERE pid <> pg_backend_pid()),
  'active', count(*) FILTER (WHERE pid <> pg_backend_pid() AND state = 'active'),
  'idle_in_transaction', count(*) FILTER (
    WHERE pid <> pg_backend_pid() AND state LIKE 'idle in transaction%'),
  'waiting', count(*) FILTER (WHERE pid <> pg_backend_pid() AND wait_event IS NOT NULL),
  'waits', coalesce(json_object_agg(wait_name, samples) FILTER (WHERE wait_name IS NOT NULL), '{}'::json))
 FROM (
   SELECT pid, state, wait_event,
          CASE WHEN wait_event IS NULL THEN NULL ELSE wait_event_type || ':' || wait_event END AS wait_name,
          count(*) OVER (PARTITION BY wait_event_type, wait_event) AS samples
   FROM pg_stat_activity WHERE datname = current_database()
 ) activity),
'lock_waits', (SELECT coalesce(json_object_agg(lock_name, samples), '{}'::json)
 FROM (
   SELECT locktype || ':' || mode AS lock_name, count(*) AS samples
   FROM pg_locks
   WHERE NOT granted AND (database IS NULL OR database = (SELECT oid FROM pg_database WHERE datname = current_database()))
   GROUP BY locktype, mode
 ) locks)
"""


def subtract_postgres_counters(before: object, after: object) -> dict[str, object]:
    if not isinstance(before, dict) or not isinstance(after, dict):
        raise ValueError("PostgreSQL counter snapshots must be objects")
    delta: dict[str, object] = {}
    for group in ("database", "wal"):
        old_values = before.get(group, {})
        new_values = after.get(group, {})
        if not isinstance(old_values, dict) or not isinstance(new_values, dict):
            raise ValueError(f"PostgreSQL {group} counter snapshot must be an object")
        delta[group] = {
            key: float(value) - float(old_values.get(key, 0))
            for key, value in new_values.items()
        }
    old_slru = before.get("slru", {})
    new_slru = after.get("slru", {})
    if not isinstance(old_slru, dict) or not isinstance(new_slru, dict):
        raise ValueError("PostgreSQL slru counter snapshot must be an object")
    delta["slru"] = {}
    for name, values in new_slru.items():
        old_values = old_slru.get(name, {})
        if not isinstance(values, dict) or not isinstance(old_values, dict):
            raise ValueError("PostgreSQL slru counter row must be an object")
        delta["slru"][name] = {
            key: float(value) - float(old_values.get(key, 0))
            for key, value in values.items() if key != "name"
        }
    return delta


class PostgresDiagnosticsCollector:
    def __init__(self, dsn: str, interval_seconds: float = POSTGRES_SAMPLE_INTERVAL_SECONDS) -> None:
        self._dsn = dsn
        self._interval_seconds = interval_seconds
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._samples: list[dict[str, object]] = []
        self._error: BaseException | None = None
        self._before: object = {}

    def __enter__(self) -> "PostgresDiagnosticsCollector":
        self._before = run_psql_json(self._dsn, POSTGRES_COUNTER_SQL)
        self._thread = threading.Thread(target=self._sample, name="postgres-diagnostics", daemon=True)
        self._thread.start()
        return self

    def _sample(self) -> None:
        while not self._stop.wait(self._interval_seconds):
            try:
                sample = run_psql_json(self._dsn, POSTGRES_ACTIVITY_SQL)
                if isinstance(sample, dict) and isinstance(sample.get("activity"), dict):
                    activity = dict(sample["activity"])
                    activity["lock_waits"] = sample.get("lock_waits", {})
                    self._samples.append(activity)
            except BaseException as error:  # Preserve failures for the orchestrating thread.
                self._error = error
                return

    def __exit__(self, exception_type: object, exception: object, traceback: object) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join()

    def result(self) -> dict[str, object]:
        if self._error is not None:
            raise ValueError(f"PostgreSQL activity sampling failed: {self._error}") from self._error
        after = run_psql_json(self._dsn, POSTGRES_COUNTER_SQL)
        waits: dict[str, int] = {}
        lock_waits: dict[str, int] = {}
        for sample in self._samples:
            sample_waits = sample.get("waits", {})
            if isinstance(sample_waits, dict):
                for name, count in sample_waits.items():
                    waits[str(name)] = waits.get(str(name), 0) + int(count)
            sample_lock_waits = sample.get("lock_waits", {})
            if isinstance(sample_lock_waits, dict):
                for name, count in sample_lock_waits.items():
                    lock_waits[str(name)] = lock_waits.get(str(name), 0) + int(count)
        return {
            "sample_interval_ms": self._interval_seconds * 1000.0,
            "sample_count": len(self._samples),
            "max_sessions": max((int(row.get("sessions", 0)) for row in self._samples), default=0),
            "max_active_writers": max((int(row.get("active", 0)) for row in self._samples), default=0),
            "max_idle_in_transaction": max(
                (int(row.get("idle_in_transaction", 0)) for row in self._samples), default=0),
            "max_waiting": max((int(row.get("waiting", 0)) for row in self._samples), default=0),
            "wait_event_samples": waits,
            "lock_wait_samples": lock_waits,
            "counter_delta": subtract_postgres_counters(self._before, after),
        }



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


def subscription_diagnostics_requested() -> bool:
    return os.environ.get("A2A_SUBSCRIPTION_DIAGNOSTICS") == "1"


def performance_build_dir(diagnostics_requested: bool) -> Path:
    build_dir = Path(os.environ.get("A2A_PERF_BUILD_DIR", DEFAULT_BUILD_DIR))
    if diagnostics_requested:
        return build_dir.with_name(f"{build_dir.name}{SUBSCRIPTION_DIAGNOSTICS_BUILD_SUFFIX}")
    return build_dir


def build_matches_subscription_diagnostics_mode(build_dir: Path, diagnostics_requested: bool) -> bool:
    cache_path = build_dir / "CMakeCache.txt"
    if not cache_path.exists():
        return False
    expected_value = "ON" if diagnostics_requested else "OFF"
    expected_entry = f"{SUBSCRIPTION_DIAGNOSTICS_CMAKE_CACHE_PREFIX}{expected_value}"
    return expected_entry in cache_path.read_text(encoding="utf-8")


def ensure_executable(config: RunnerConfig, env_name: str, target_name: str, description: str) -> Path:
    explicit = os.environ.get(env_name)
    if explicit:
        executable = Path(explicit)
        if not executable.exists():
            raise ValueError(f"{env_name} does not exist: {executable}")
        return executable
    diagnostics_requested = subscription_diagnostics_requested()
    build_dir = performance_build_dir(diagnostics_requested)
    executable = executable_path_from_build(build_dir, target_name)
    if executable.exists() and build_matches_subscription_diagnostics_mode(build_dir, diagnostics_requested):
        return executable
    configure = [
        "cmake", "-S", ".", "-B", str(build_dir), "-DCMAKE_BUILD_TYPE=Release",
        "-DA2A_ENABLE_TESTING=ON",
        f"-DA2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS={'ON' if diagnostics_requested else 'OFF'}",
    ]
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
    # Do not ask the kernel for an ephemeral port here: after the probes close,
    # that port can immediately be reused before tck_sut binds its listeners.
    # Randomizing the first pair also prevents parallel runners from all probing
    # the same free pair before any tck_sut process has had a chance to bind it.
    start_pair = secrets.randbelow(SUT_PORT_PAIR_COUNT)
    for offset in range(SUT_PORT_PAIR_COUNT):
        pair = (start_pair + offset) % SUT_PORT_PAIR_COUNT
        port = SUT_PORT_RANGE_START + (pair * SUT_PORT_PAIR_STEP)
        with (socket.socket(socket.AF_INET, socket.SOCK_STREAM) as http_probe,
              socket.socket(socket.AF_INET, socket.SOCK_STREAM) as grpc_probe):
            try:
                http_probe.bind((host, port))
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


def read_http_diagnostics(path: Path) -> dict[str, int | float]:
    if not path.exists():
        return {}
    matches = HTTP_DIAGNOSTICS_PATTERN.findall(path.read_text(encoding="utf-8", errors="replace"))
    if not matches:
        return {}
    accepted, completed, reuse, finite_connections, completed_streams, streams_per_connection, reused_after_stream = (
        matches[-1]
    )
    return {
        "http_coordinate_accepted_connections": int(accepted),
        "http_coordinate_completed_unary_operations": int(completed),
        "http_coordinate_operations_per_connection": float(reuse),
        "http_finite_stream_connections": int(finite_connections),
        "http_completed_finite_streams": int(completed_streams),
        "http_finite_streams_per_connection": float(streams_per_connection),
        "http_connections_reused_after_finite_stream": int(reused_after_stream),
    }


def read_subscription_diagnostics(path: Path) -> dict[str, dict[str, int]]:
    if not path.exists():
        return {}
    lines = [line for line in path.read_text(encoding="utf-8", errors="replace").splitlines()
             if line.startswith(SUBSCRIPTION_DIAGNOSTICS_PREFIX)]
    if not lines:
        return {}
    fields = dict(field.split("=", 1) for field in lines[-1].split()[1:])
    return {
        phase: {
            "count": int(fields[f"{phase}_count"]),
            "total_ns": int(fields[f"{phase}_total_ns"]),
            "max_ns": int(fields[f"{phase}_max_ns"]),
        }
        for phase in SUBSCRIPTION_DIAGNOSTIC_PHASES
    }


def postgres_schema_name(transport: str, concurrency: int, port: int) -> str:
    safe_transport = "".join(ch if ch.isalnum() else "_" for ch in transport.lower())
    return f"a2a_perf_{safe_transport}_{concurrency}_{port}"


class SutProcess:
    def __init__(self, config: RunnerConfig, store_backend: str, port: int, transport: str,
                 concurrency: int, postgres_pool_size: int) -> None:
        self.host = "127.0.0.1"
        self.port = port
        self.grpc_port = port + 1
        self.log_path = config.report_dir / f"tck_sut_{store_backend}_{port}.log"
        self.process: subprocess.Popen[str] | None = None
        self.sut = ensure_sut(config)
        self.store_backend = store_backend
        self.transport = transport
        self.concurrency = concurrency
        self.postgres_pool_size = postgres_pool_size

    def __enter__(self) -> "SutProcess":
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        env = os.environ.copy()
        env["A2A_TCK_STORE_BACKEND"] = self.store_backend
        if self.store_backend == "postgres":
            if "A2A_TCK_POSTGRES_DSN" not in env and "A2A_TEST_POSTGRES_DSN" in env:
                env["A2A_TCK_POSTGRES_DSN"] = env["A2A_TEST_POSTGRES_DSN"]
            env["A2A_TCK_POSTGRES_SCHEMA"] = postgres_schema_name(self.transport, self.concurrency, self.port)
            env["A2A_TCK_POSTGRES_POOL_SIZE"] = str(self.postgres_pool_size)
        log_file = self.log_path.open("w", encoding="utf-8")
        creation_flags = subprocess.CREATE_NEW_PROCESS_GROUP if sys.platform == "win32" else 0
        self.process = subprocess.Popen(
            [str(self.sut), f"{self.host}:{self.port}"],
            cwd=Path(__file__).resolve().parents[1],
            env=env,
            stdout=log_file,
            stderr=subprocess.STDOUT,
            text=True,
            creationflags=creation_flags,
        )
        log_file.close()
        wait_for_port(self.host, self.port, self.process, self.log_path)
        wait_for_port(self.host, self.grpc_port, self.process, self.log_path)
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        if self.process is None:
            return
        if self.process.poll() is None:
            if sys.platform == "win32":
                os.kill(self.process.pid, signal.CTRL_BREAK_EVENT)
            else:
                self.process.terminate()
            try:
                self.process.wait(timeout=SUT_GRACEFUL_SHUTDOWN_TIMEOUT_SECONDS)
            except subprocess.TimeoutExpired as timeout_error:
                self.process.kill()
                self.process.wait(timeout=SUT_FORCE_KILL_TIMEOUT_SECONDS)
                coordinate = f"{self.transport}/{self.store_backend}/c{self.concurrency}"
                raise ValueError(
                    f"tck_sut failed to terminate gracefully for {coordinate}; logs:\n{read_tail(self.log_path)}"
                ) from timeout_error


def run_command_json(command: list[str], timeout_seconds: float, error_context: str,
                     log_path: Path | None = None,
                     env: dict[str, str] | None = None) -> list[dict[str, object]]:
    process = subprocess.Popen(
        command,
        cwd=Path(__file__).resolve().parents[1],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
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


def run_driver(config: RunnerConfig, transport: str, store_backend: str, concurrency: int,
               postgres_pool_size: int, scenarios: tuple[str, ...] | None = None,
               schema: str | None = None) -> list[dict[str, object]]:
    driver = ensure_driver(config)
    command = [
        str(driver),
        "--transport", transport,
        "--store-backend", store_backend,
        "--requests", str(config.requests),
        "--concurrency", str(concurrency),
        "--push-config-fanout", str(config.push_config_fanout),
        "--warmup-seconds", str(config.warmup_seconds),
        "--duration-seconds", str(config.duration_seconds),
    ]
    if scenarios is not None:
        command.extend(["--scenarios", ",".join(scenarios)])
    env = os.environ.copy()
    env["A2A_PERF_POSTGRES_POOL_SIZE"] = str(postgres_pool_size)
    if schema is not None:
        env["A2A_PERF_POSTGRES_SCHEMA"] = schema
    payload = run_command_json(command, config.driver_timeout_seconds,
                               f"performance driver for {transport}/{store_backend}/c{concurrency}", env=env)
    for result in payload:
        result["postgres_pool_size"] = postgres_pool_size if store_backend == "postgres" else None
    return payload



def run_wire_driver(config: RunnerConfig, transport: str, store_backend: str, concurrency: int,
                    postgres_pool_size: int, port: int) -> list[dict[str, object]]:
    if transport not in WIRE_TRANSPORT_PATHS:
        raise ValueError(f"unsupported wire transport: {transport}")
    wire_scenarios = wire_scenarios_for_transport(transport, config.scenarios)
    if not wire_scenarios:
        return []
    wire_driver = ensure_wire_driver(config)
    with SutProcess(config, store_backend, port, transport, concurrency, postgres_pool_size) as sut:
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
            "--scenarios", ",".join(wire_scenarios),
        ]
        payload = run_command_json(
            command, config.wire_driver_timeout_seconds,
            f"wire performance driver for {transport}/{store_backend}/c{concurrency}", sut.log_path,
        )
    diagnostics = read_http_diagnostics(sut.log_path) if transport in {"http_json", "jsonrpc"} else {}
    server_subscription_diagnostics = read_subscription_diagnostics(sut.log_path)
    if os.environ.get("A2A_SUBSCRIPTION_DIAGNOSTICS") == "1" and not server_subscription_diagnostics:
        raise ValueError("subscription diagnostics were requested but the SUT did not report server diagnostics")
    if transport in {"http_json", "jsonrpc"} and not diagnostics:
        raise ValueError(
            f"wire performance driver for {transport}/{store_backend}/c{concurrency} "
            "did not emit HTTP reuse diagnostics"
        )
    for result in payload:
        if result.get("driver_type") != "wire_tck_sut" or result.get("transport_path") != WIRE_TRANSPORT_PATHS[transport]:
            raise ValueError("wire performance driver returned misleading metadata")
        result["postgres_pool_size"] = postgres_pool_size if store_backend == "postgres" else None
        if os.environ.get("A2A_SUBSCRIPTION_DIAGNOSTICS") == "1" and "client_subscription_diagnostics" not in result:
            raise ValueError("subscription diagnostics were requested but the wire driver did not report client diagnostics")
        if diagnostics:
            result.update(diagnostics)
        if server_subscription_diagnostics:
            result["server_subscription_diagnostics"] = server_subscription_diagnostics
    return payload


def wire_scenarios_for_transport(transport: str, scenarios: tuple[str, ...] | None = None) -> tuple[str, ...]:
    del transport
    if scenarios is None:
        return WIRE_SCENARIOS
    return tuple(scenario for scenario in scenarios if scenario in WIRE_SCENARIOS)


def in_process_scenarios(scenarios: tuple[str, ...] | None = None) -> tuple[str, ...]:
    if scenarios is None:
        return IN_PROCESS_SCENARIOS
    return tuple(scenario for scenario in scenarios if scenario in IN_PROCESS_SCENARIOS)


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


def parse_positive_int_csv(value: str, selection_name: str) -> tuple[int, ...]:
    levels = tuple(int(item.strip()) for item in value.split(",") if item.strip())
    if not levels or any(level <= 0 for level in levels):
        raise ValueError(f"{selection_name} must be positive integers")
    if len(levels) != len(set(levels)):
        raise ValueError(f"{selection_name} must not contain duplicates")
    return levels


def env_or_default(name: str, default: str) -> str:
    return os.environ.get(name, default)


def parse_args(argv: list[str]) -> RunnerConfig:
    parser = argparse.ArgumentParser(description="Run report-only A2A performance scenarios.")
    parser.add_argument("--profile", choices=POSTGRES_PROFILES)
    parser.add_argument("--transports", default=env_or_default("A2A_PERF_TRANSPORTS", ",".join(TRANSPORTS)))
    parser.add_argument("--store-backends", default=env_or_default("A2A_PERF_STORE_BACKENDS", ",".join(STORE_BACKENDS)))
    parser.add_argument("--requests", type=int, default=int(env_or_default("A2A_PERF_REQUESTS", str(DEFAULT_REQUESTS))))
    parser.add_argument("--concurrency", default=env_or_default("A2A_PERF_CONCURRENCY", ",".join(str(level) for level in DEFAULT_CONCURRENCY)))
    parser.add_argument("--push-config-fanout", type=int,
                        default=int(env_or_default("A2A_PERF_PUSH_CONFIG_FANOUT", str(DEFAULT_PUSH_CONFIG_FANOUT))))
    parser.add_argument("--scenarios", default=os.environ.get("A2A_PERF_SCENARIOS"))
    parser.add_argument("--warmup-seconds", type=float, default=float(env_or_default("A2A_PERF_WARMUP_SECONDS", str(DEFAULT_WARMUP_SECONDS))))
    parser.add_argument("--duration-seconds", type=float, default=float(env_or_default("A2A_PERF_DURATION_SECONDS", str(DEFAULT_DURATION_SECONDS))))
    parser.add_argument("--report-dir", default=env_or_default("A2A_PERF_REPORT_DIR", DEFAULT_REPORT_DIR))
    parser.add_argument("--postgres-pool-sizes", default=os.environ.get("A2A_PERF_POSTGRES_POOL_SIZES"))
    parser.add_argument("--driver-timeout-seconds", type=float, default=float(env_or_default("A2A_PERF_DRIVER_TIMEOUT_SECONDS", str(DEFAULT_DRIVER_TIMEOUT_SECONDS))))
    parser.add_argument("--wire-driver-timeout-seconds", type=float, default=float(env_or_default("A2A_PERF_WIRE_DRIVER_TIMEOUT_SECONDS", str(DEFAULT_WIRE_DRIVER_TIMEOUT_SECONDS))))
    args = parser.parse_args(argv)
    if args.requests <= 0:
        raise ValueError("requests must be positive")
    if args.push_config_fanout <= 0:
        raise ValueError("push-config fanout must be positive")
    if args.warmup_seconds < 0 or args.duration_seconds < 0:
        raise ValueError("durations must be non-negative")
    if args.driver_timeout_seconds <= 0 or args.wire_driver_timeout_seconds <= 0:
        raise ValueError("driver timeouts must be positive")
    profile = args.profile
    is_postgres_profile = profile in POSTGRES_PROFILES
    if args.postgres_pool_sizes is not None:
        postgres_pool_sizes = parse_positive_int_csv(args.postgres_pool_sizes, "PostgreSQL pool sizes")
    elif profile == POSTGRES_TAIL_PROFILE:
        postgres_pool_sizes = POSTGRES_TAIL_POOL_SIZES
    elif profile == POSTGRES_TAIL_C1_PROFILE:
        postgres_pool_sizes = POSTGRES_TAIL_C1_POOL_SIZES
    elif profile == POSTGRES_WRITE_PROFILE:
        postgres_pool_sizes = POSTGRES_WRITE_POOL_SIZES
    else:
        postgres_pool_sizes = (DEFAULT_POSTGRES_POOL_SIZE,)
    if profile == POSTGRES_TAIL_PROFILE:
        concurrency_levels = POSTGRES_TAIL_CONCURRENCY
        scenarios = POSTGRES_TAIL_SCENARIOS
    elif profile == POSTGRES_TAIL_C1_PROFILE:
        concurrency_levels = POSTGRES_TAIL_C1_CONCURRENCY
        scenarios = POSTGRES_TAIL_C1_SCENARIOS
    elif profile == POSTGRES_WRITE_PROFILE:
        concurrency_levels = POSTGRES_WRITE_CONCURRENCY
        scenarios = POSTGRES_WRITE_SCENARIOS
    else:
        concurrency_levels = parse_positive_int_csv(args.concurrency, "concurrency levels")
        scenarios = None if args.scenarios is None else split_csv(args.scenarios, SCENARIOS)
    return RunnerConfig(
        profile=profile,
        transports=("grpc",) if is_postgres_profile else split_csv(args.transports, TRANSPORTS),
        store_backends=("postgres",) if is_postgres_profile else split_csv(args.store_backends, STORE_BACKENDS),
        requests=DEFAULT_REQUESTS if is_postgres_profile else args.requests,
        push_config_fanout=args.push_config_fanout,
        concurrency_levels=concurrency_levels,
        warmup_seconds=DEFAULT_WARMUP_SECONDS if is_postgres_profile else args.warmup_seconds,
        duration_seconds=DEFAULT_DURATION_SECONDS if is_postgres_profile else args.duration_seconds,
        report_dir=Path(args.report_dir),
        driver_timeout_seconds=args.driver_timeout_seconds,
        wire_driver_timeout_seconds=args.wire_driver_timeout_seconds,
        repetitions=(POSTGRES_WRITE_REPETITIONS if profile == POSTGRES_WRITE_PROFILE
                     else POSTGRES_TAIL_REPETITIONS if profile in POSTGRES_TAIL_PROFILES else 1),
        scenarios=scenarios,
        postgres_pool_sizes=postgres_pool_sizes,
    )


def commit_sha() -> str:
    try:
        return subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def host_metadata() -> dict[str, str]:
    return {"os": platform.platform(), "cpu": platform.processor() or platform.machine(), "python": platform.python_version()}


def write_reports(results: list[dict[str, object]], config: RunnerConfig,
                  aggregates: list[dict[str, object]] | None = None,
                  query_plans: str | None = None) -> None:
    config.report_dir.mkdir(parents=True, exist_ok=True)
    metadata = {
        "sdk_commit_sha": commit_sha(),
        "host": host_metadata(),
        "push_config_fanout": config.push_config_fanout,
    }
    payload: dict[str, object] = {"metadata": metadata, "results": results}
    if aggregates is not None:
        payload["median_aggregates"] = aggregates
    if query_plans is not None:
        payload["query_plans"] = query_plans
    (config.report_dir / "results.json").write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    write_csv(results, config.report_dir / "results.csv")
    if aggregates is not None:
        write_aggregate_csv(aggregates, config.report_dir / "median-aggregates.csv")
    summary = render_markdown_summary(results, metadata, aggregates, query_plans)
    (config.report_dir / "summary.md").write_text(summary + "\n", encoding="utf-8")


def write_csv(results: list[dict[str, object]], csv_path: Path) -> None:
    fieldnames = ["repetition", "scenario", "transport", "store_backend", "driver_type", "transport_path", "concurrency", "postgres_pool_size", "operations", "success", "errors", "throughput_ops_per_sec", "configured_requests", "push_config_fanout", "configured_duration_seconds", "measured_duration_seconds", "history_depth", "successful_deliveries", "failed_deliveries", "callback_count", "event_count", "first_event_p50_ms", "first_event_p95_ms", "stream_completion_p50_ms", "stream_completion_p95_ms", "fanout_per_operation", "total_fanout_count", "fanout_count", "p50_ms", "p90_ms", "p95_ms", "p99_ms", "max_ms"]
    fieldnames.extend((
        "http_coordinate_accepted_connections",
        "http_coordinate_completed_unary_operations",
        "http_coordinate_operations_per_connection",
        "http_finite_stream_connections",
        "http_completed_finite_streams",
        "http_finite_streams_per_connection",
        "http_connections_reused_after_finite_stream",
    ))
    for phase in POSTGRES_DIAGNOSTIC_PHASES:
        fieldnames.extend((f"{phase}_p95_ms", f"{phase}_p99_ms", f"{phase}_max_ms",
                           f"{phase}_call_count", f"{phase}_calls_per_operation"))
    with csv_path.open("w", encoding="utf-8", newline="") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=fieldnames)
        writer.writeheader()
        for result in results:
            latency = result["latency_ms"]
            assert isinstance(latency, dict)
            row = {key: result.get(key, 0) for key in fieldnames}
            first_event_latency = result.get("first_event_latency_ms", {})
            if not isinstance(first_event_latency, dict):
                first_event_latency = {}
            completion_latency = result.get("stream_completion_latency_ms", {})
            if not isinstance(completion_latency, dict):
                completion_latency = {}
            row.update({"p50_ms": latency["p50"], "p90_ms": latency["p90"], "p95_ms": latency["p95"], "p99_ms": latency["p99"], "max_ms": latency["max"]})
            row.update({
                "first_event_p50_ms": first_event_latency.get("p50", 0),
                "first_event_p95_ms": first_event_latency.get("p95", 0),
                "stream_completion_p50_ms": completion_latency.get("p50", 0),
                "stream_completion_p95_ms": completion_latency.get("p95", 0),
            })
            phase_latencies = result.get("postgres_phase_latency_ms", {})
            phase_call_counts = result.get("postgres_phase_call_count", {})
            phase_calls_per_operation = result.get("postgres_phase_calls_per_operation", {})
            if not isinstance(phase_latencies, dict):
                phase_latencies = {}
            for phase in POSTGRES_DIAGNOSTIC_PHASES:
                phase_values = phase_latencies.get(phase, {})
                if not isinstance(phase_values, dict):
                    phase_values = {}
                row.update({
                    f"{phase}_p95_ms": phase_values.get("p95", 0),
                    f"{phase}_p99_ms": phase_values.get("p99", 0),
                    f"{phase}_max_ms": phase_values.get("max", 0),
                    f"{phase}_call_count": phase_call_counts.get(phase, 0),
                    f"{phase}_calls_per_operation": phase_calls_per_operation.get(phase, 0),
                })
            writer.writerow(row)


def store_label(store: str) -> str:
    return {"inmemory": "In-memory", "postgres": "PostgreSQL"}.get(store, store)


def path_label(path: str) -> str:
    return "In-process" if path == "in_process" else "Wire"


def transport_label(result: dict[str, object]) -> str:
    if result["transport_path"] == "in_process":
        return "—"
    return {"grpc": "gRPC", "http_json": "HTTP+JSON", "jsonrpc": "JSON-RPC"}.get(
        str(result["transport"]), str(result["transport"])
    )


def append_collapsible_start(lines: list[str], summary: str) -> None:
    lines.extend(["", "<details>", f"<summary>{summary}</summary>"])


def append_collapsible_end(lines: list[str]) -> None:
    lines.extend(["", "</details>"])


def result_groups(results: list[dict[str, object]]) -> list[tuple[tuple[str, str, str, int | None], list[dict[str, object]]]]:
    groups: dict[tuple[str, str, str, int | None], list[dict[str, object]]] = {}
    for result in results:
        path = "in_process" if result["transport_path"] == "in_process" else "wire"
        transport = "" if path == "in_process" else str(result["transport"])
        pool_size = int(result["postgres_pool_size"]) if result.get("postgres_pool_size") is not None else None
        key = (str(result["store_backend"]), path, transport, pool_size)
        groups.setdefault(key, []).append(result)
    return [(key, groups[key]) for key in sorted(groups, key=lambda item: tuple(str(value) for value in item))]


def append_pivot_table(lines: list[str], results: list[dict[str, object]]) -> None:
    concurrency_levels = sorted({int(result["concurrency"]) for result in results})
    header = "| Scenario | " + " | ".join(
        value for concurrency in concurrency_levels
        for value in (f"c{concurrency} ops/s", f"c{concurrency} p95 ms")
    ) + " |"
    lines.extend(["", header, "| --- | " + " | ".join("---:" for _ in range(2 * len(concurrency_levels))) + " |"])
    by_coordinate = {(str(result["scenario"]), int(result["concurrency"])): result for result in results}
    for scenario in sorted({str(result["scenario"]) for result in results}):
        values = []
        for concurrency in concurrency_levels:
            result = by_coordinate.get((scenario, concurrency))
            if result is None:
                values.extend(("—", "—"))
                continue
            latency = result["latency_ms"]
            assert isinstance(latency, dict)
            values.extend((f"{float(result['throughput_ops_per_sec']):.2f}", f"{float(latency['p95']):.4f}"))
        lines.append(f"| {scenario} | " + " | ".join(values) + " |")


def append_grouped_result_sections(lines: list[str], results: list[dict[str, object]]) -> None:
    for (store, path, transport, pool_size), selected in result_groups(results):
        title = f"{path_label(path)} results — {store_label(store)}"
        if path == "wire":
            title += f" — {transport_label(selected[0])}"
        if store == "postgres" and pool_size is not None:
            title += f" — pool {pool_size}"
        lines.extend(["", f"## {title}"])
        append_collapsible_start(lines, f"Show {title}")
        append_pivot_table(lines, selected)
        append_collapsible_end(lines)


def safe_ratio(high: float, low: float) -> float | None:
    return None if low == 0 else high / low


def cross_backend_scaling(results: list[dict[str, object]]) -> list[dict[str, object]]:
    coordinates: dict[tuple[str, str, str, int | None], dict[int, dict[str, object]]] = {}
    for result in results:
        path = "in_process" if result["transport_path"] == "in_process" else "wire"
        transport = "" if path == "in_process" else str(result["transport"])
        pool = int(result["postgres_pool_size"]) if result.get("postgres_pool_size") is not None else None
        key = (str(result["scenario"]), path, transport, pool)
        coordinates.setdefault(key, {})[int(result["concurrency"])] = result
    signals = []
    postgres_keys = [key for key in coordinates if key[3] is not None]
    for scenario, path, transport, pool in sorted(postgres_keys):
        postgres_rows = coordinates[(scenario, path, transport, pool)]
        memory_rows = coordinates.get((scenario, path, transport, None), {})
        levels = sorted(set(postgres_rows) & set(memory_rows))
        if len(levels) < 2:
            continue
        low, high = levels[0], levels[-1]
        memory_low, memory_high = memory_rows[low], memory_rows[high]
        postgres_low, postgres_high = postgres_rows[low], postgres_rows[high]
        signals.append({
            "scenario": scenario, "path": path, "transport": transport, "pool_size": pool,
            "low": low, "high": high,
            "memory_p95_growth": metric_ratio(memory_low, memory_high, "p95"),
            "postgres_p95_growth": metric_ratio(postgres_low, postgres_high, "p95"),
            "memory_throughput_scaling": safe_ratio(float(memory_high["throughput_ops_per_sec"]), float(memory_low["throughput_ops_per_sec"])),
            "postgres_throughput_scaling": safe_ratio(float(postgres_high["throughput_ops_per_sec"]), float(postgres_low["throughput_ops_per_sec"])),
        })
    return signals


def metric_ratio(low: dict[str, object], high: dict[str, object], metric: str) -> float | None:
    low_latency, high_latency = low["latency_ms"], high["latency_ms"]
    assert isinstance(low_latency, dict) and isinstance(high_latency, dict)
    return safe_ratio(float(high_latency[metric]), float(low_latency[metric]))


def format_ratio(value: object) -> str:
    return "n/a" if value is None else f"{float(value):.2f}×"


def append_cross_backend_markdown(lines: list[str], signals: list[dict[str, object]]) -> None:
    lines.extend(["", "## Cross-backend scaling signals", "",
                  "Relative scaling between the lowest and highest shared concurrency; this is diagnostic, not a pass/fail verdict."])
    append_collapsible_start(lines, "Show cross-backend scaling signals")
    lines.extend(["",
                  "| Scenario | Path | Transport | PostgreSQL pool | Concurrency range | In-memory p95 growth | PostgreSQL p95 growth | In-memory throughput scaling | PostgreSQL throughput scaling |",
                  "| --- | --- | --- | ---: | --- | ---: | ---: | ---: | ---: |"])
    if not signals:
        lines.append("| _No comparable coordinates_ | — | — | — | — | n/a | n/a | n/a | n/a |")
    for row in signals:
        transport = "—" if row["path"] == "in_process" else transport_label(
            {"transport_path": "wire", "transport": row["transport"]}
        )
        lines.append(
            f"| {row['scenario']} | {path_label(str(row['path']))} | {transport} | {row['pool_size']} | "
            f"c{row['low']}–c{row['high']} | {format_ratio(row['memory_p95_growth'])} | "
            f"{format_ratio(row['postgres_p95_growth'])} | {format_ratio(row['memory_throughput_scaling'])} | "
            f"{format_ratio(row['postgres_throughput_scaling'])} |"
        )
    append_collapsible_end(lines)


def append_detailed_matrix(lines: list[str], results: list[dict[str, object]]) -> None:
    repeated = any(result.get("repetition") is not None for result in results)
    repetition_header = " Repetition |" if repeated else ""
    repetition_rule = " ---: |" if repeated else ""
    lines.extend(["", "## Detailed matrix results", "", "<details>", "<summary>Show all raw result rows</summary>", "",
                  f"|{repetition_header} Scenario | Driver | Path | Transport | Store | Concurrency | Pool size | Success | Errors | Ops/sec | p50 ms | p95 ms | p99 ms | Max ms |",
                  f"|{repetition_rule} --- | --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"])
    for result in results:
        latency = result["latency_ms"]
        assert isinstance(latency, dict)
        repetition = f" {result['repetition']} |" if repeated else ""
        lines.append(
            f"|{repetition} {result['scenario']} | {result['driver_type']} | {path_label(str(result['transport_path']))} | "
            f"{transport_label(result)} | {store_label(str(result['store_backend']))} | {result['concurrency']} | "
            f"{result.get('postgres_pool_size') or ''} | {result['success']} | {result['errors']} | "
            f"{float(result['throughput_ops_per_sec']):.2f} | {float(latency['p50']):.4f} | "
            f"{float(latency['p95']):.4f} | {float(latency['p99']):.4f} | {float(latency['max']):.4f} |"
        )
    lines.extend(["", "</details>"])


def render_markdown_summary(results: list[dict[str, object]], metadata: dict[str, object],
                            aggregates: list[dict[str, object]] | None = None,
                            query_plans: str | None = None) -> str:
    host = metadata["host"]
    assert isinstance(host, dict)
    lines = [
        "# A2A performance test summary",
        "",
        f"* SDK commit: `{metadata['sdk_commit_sha']}`",
        f"* Host: {host['os']} ({host['cpu']})",
        f"* Result rows: {len(results)}",
        f"* Push-config fanout fixture: {metadata.get('push_config_fanout', DEFAULT_PUSH_CONFIG_FANOUT)}",
        "* Mode: report-only; no performance thresholds are enforced.",
        "",
        "## Execution summary",
        "",
        "| Store | Path | Rows | Operations | Success | Errors |",
        "| --- | --- | ---: | ---: | ---: | ---: |",
    ]
    for row in execution_rollups(results):
        lines.append(
            f"| {store_label(str(row['store']))} | {path_label(str(row['path']))} | {row['rows']} | "
            f"{row['operations']} | {row['success']} | {row['errors']} |"
        )
    if aggregates is None:
        append_grouped_result_sections(lines, results)
        append_cross_backend_markdown(lines, cross_backend_scaling(results))
    append_detailed_matrix(lines, results)
    diagnostic_results = [result for result in results if "postgres_phase_latency_ms" in result]
    if diagnostic_results:
        lines.extend([
            "",
            "## PostgreSQL phase diagnostics",
        ])
        append_collapsible_start(lines, "Show PostgreSQL phase diagnostics")
        lines.extend([
            "",
            "| Scenario | Transport | Concurrency | Pool size | Phase | p95 ms | p99 ms | Max ms | Calls | Calls/op |",
            "| --- | --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: |",
        ])
        for result in diagnostic_results:
            phases = result["postgres_phase_latency_ms"]
            assert isinstance(phases, dict)
            for phase in POSTGRES_DIAGNOSTIC_PHASES:
                values = phases[phase]
                assert isinstance(values, dict)
                call_count = int(result.get('postgres_phase_call_count', {}).get(phase, 0))
                if "postgres_phase_call_count" in result and call_count == 0:
                    continue
                lines.append(
                    f"| {result['scenario']} | {transport_label(result)} | {result['concurrency']} | {result['postgres_pool_size']} | {phase} | "
                    f"{float(values['p95']):.4f} | {float(values['p99']):.4f} | {float(values['max']):.4f} | "
                    f"{call_count} | "
                    f"{float(result.get('postgres_phase_calls_per_operation', {}).get(phase, 0)):.4f} |"
                )
        append_collapsible_end(lines)
    if aggregates is not None:
        append_aggregate_markdown(lines, aggregates)
    if query_plans is not None:
        lines.extend(["", "## Query-plan review", "", "```text", query_plans.rstrip(), "```"])
    return "\n".join(lines)


def execution_rollups(results: list[dict[str, object]]) -> list[dict[str, object]]:
    rollups: dict[tuple[str, str], dict[str, object]] = {}
    for result in results:
        path = "in_process" if result["transport_path"] == "in_process" else "wire"
        key = (str(result["store_backend"]), path)
        rollup = rollups.setdefault(key, {"store": key[0], "path": key[1], "rows": 0,
                                          "operations": 0, "success": 0, "errors": 0})
        rollup["rows"] = int(rollup["rows"]) + 1
        rollup["operations"] = int(rollup["operations"]) + int(result["operations"])
        rollup["success"] = int(rollup["success"]) + int(result["success"])
        rollup["errors"] = int(rollup["errors"]) + int(result["errors"])
    return [rollups[key] for key in sorted(rollups)]


def median_aggregates(results: list[dict[str, object]], repetitions: int) -> list[dict[str, object]]:
    aggregates = []
    keys = sorted({(str(row["scenario"]), int(row["concurrency"]), int(row["postgres_pool_size"])) for row in results})
    for scenario, concurrency, pool_size in keys:
        selected = [row for row in results
                    if row["scenario"] == scenario and int(row["concurrency"]) == concurrency
                    and int(row["postgres_pool_size"]) == pool_size]
        if len(selected) != repetitions:
            raise ValueError(
                f"aggregate {scenario}/c{concurrency} contains {len(selected)}/{repetitions} repetitions"
            )
        aggregate: dict[str, object] = {
            "scenario": scenario,
            "concurrency": concurrency,
            "postgres_pool_size": pool_size,
            "repetitions": len(selected),
            "throughput_ops_per_sec": statistics.median(
                float(row["throughput_ops_per_sec"]) for row in selected
            ),
        }
        for metric in ("p95", "p99", "max"):
            aggregate[f"{metric}_ms"] = statistics.median(
                float(row["latency_ms"][metric]) for row in selected  # type: ignore[index]
            )
        phase_medians = {}
        for phase in POSTGRES_DIAGNOSTIC_PHASES:
            phase_medians[phase] = {
                metric: statistics.median(
                    float(row["postgres_phase_latency_ms"][phase][metric])  # type: ignore[index]
                    for row in selected
                )
                for metric in ("p95", "p99", "max")
            }
        aggregate["postgres_phase_latency_ms"] = phase_medians
        aggregate["postgres_phase_call_count"] = {
            phase: statistics.median(
                float(row.get("postgres_phase_call_count", {}).get(phase, 0)) for row in selected
            )
            for phase in POSTGRES_DIAGNOSTIC_PHASES
        }
        aggregate["postgres_phase_calls_per_operation"] = {
            phase: statistics.median(
                float(row.get("postgres_phase_calls_per_operation", {}).get(phase, 0)) for row in selected
            )
            for phase in POSTGRES_DIAGNOSTIC_PHASES
        }
        aggregates.append(aggregate)
    return aggregates


def write_aggregate_csv(aggregates: list[dict[str, object]], csv_path: Path) -> None:
    fieldnames = ["scenario", "concurrency", "postgres_pool_size", "repetitions", "throughput_ops_per_sec",
                  "p95_ms", "p99_ms", "max_ms"]
    for phase in POSTGRES_DIAGNOSTIC_PHASES:
        fieldnames.extend(f"{phase}_{metric}_ms" for metric in ("p95", "p99", "max"))
        fieldnames.extend((f"{phase}_call_count", f"{phase}_calls_per_operation"))
    with csv_path.open("w", encoding="utf-8", newline="") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=fieldnames)
        writer.writeheader()
        for aggregate in aggregates:
            row = {key: aggregate.get(key, 0) for key in fieldnames}
            phases = aggregate["postgres_phase_latency_ms"]
            assert isinstance(phases, dict)
            for phase in POSTGRES_DIAGNOSTIC_PHASES:
                for metric in ("p95", "p99", "max"):
                    row[f"{phase}_{metric}_ms"] = phases[phase][metric]
                row[f"{phase}_call_count"] = aggregate["postgres_phase_call_count"][phase]
                row[f"{phase}_calls_per_operation"] = aggregate["postgres_phase_calls_per_operation"][phase]
            writer.writerow(row)


def append_aggregate_markdown(lines: list[str], aggregates: list[dict[str, object]]) -> None:
    lines.extend(["", "## Median aggregates"])
    append_collapsible_start(lines, "Show median aggregates")
    lines.extend(["",
                  "| Scenario | Concurrency | Pool size | Repetitions | Ops/sec | p95 ms | p99 ms | Max ms |",
                  "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"])
    for row in aggregates:
        lines.append(
            f"| {row['scenario']} | {row['concurrency']} | {row['postgres_pool_size']} | {row['repetitions']} | "
            f"{float(row['throughput_ops_per_sec']):.2f} | {float(row['p95_ms']):.4f} | "
            f"{float(row['p99_ms']):.4f} | {float(row['max_ms']):.4f} |"
        )
    append_collapsible_end(lines)
    lines.extend(["", "## Median PostgreSQL phases"])
    append_collapsible_start(lines, "Show median PostgreSQL phases")
    lines.extend(["",
                  "| Scenario | Concurrency | Pool size | Phase | p95 ms | p99 ms | Max ms | Calls | Calls/op |",
                  "| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: |"])
    for row in aggregates:
        phases = row["postgres_phase_latency_ms"]
        assert isinstance(phases, dict)
        for phase in POSTGRES_DIAGNOSTIC_PHASES:
            values = phases[phase]
            lines.append(
                f"| {row['scenario']} | {row['concurrency']} | {row['postgres_pool_size']} | {phase} | "
                f"{float(values['p95']):.4f} | {float(values['p99']):.4f} | {float(values['max']):.4f} | "
                f"{float(row['postgres_phase_call_count'][phase]):.1f} | "
                f"{float(row['postgres_phase_calls_per_operation'][phase]):.4f} |"
            )
    append_collapsible_end(lines)


def explain_postgres_queries(dsn: str, schema: str, push_config_fanout: int = DEFAULT_PUSH_CONFIG_FANOUT) -> str:
    fixture_fanout = max(push_config_fanout - 1, 0)
    sql = f'''SET search_path TO "{schema}";
BEGIN;
INSERT INTO a2a_tasks (id, context_id, state, task_proto)
VALUES ('{POSTGRES_QUERY_PLAN_TASK_ID}', '{POSTGRES_QUERY_PLAN_TASK_ID}', 0, decode('', 'hex'))
ON CONFLICT (id) DO NOTHING;
INSERT INTO a2a_push_notification_configs (task_id, config_id, url, config_proto)
SELECT '{POSTGRES_QUERY_PLAN_TASK_ID}',
       '{POSTGRES_QUERY_PLAN_CONFIG_ID}-' || generated.config_number::text,
       '{POSTGRES_QUERY_PLAN_URL}', decode('', 'hex')
FROM generate_series(1, {fixture_fanout}) AS generated(config_number)
ON CONFLICT (task_id, config_id) DO NOTHING;
EXPLAIN (ANALYZE, BUFFERS, WAL)
INSERT INTO a2a_tasks AS target
  (id, context_id, state, has_status_timestamp, status_seconds, status_nanos, task_proto, updated_at)
VALUES ('{POSTGRES_QUERY_PLAN_TASK_ID}', '{POSTGRES_QUERY_PLAN_TASK_ID}', 0, false, 0, 0,
        decode('', 'hex'), now())
ON CONFLICT (id) DO UPDATE SET context_id = EXCLUDED.context_id, state = EXCLUDED.state,
  has_status_timestamp = EXCLUDED.has_status_timestamp, status_seconds = EXCLUDED.status_seconds,
  status_nanos = EXCLUDED.status_nanos, task_proto = EXCLUDED.task_proto,
  revision = target.revision + 1, updated_at = now();
EXPLAIN (ANALYZE, BUFFERS, WAL)
INSERT INTO a2a_push_notification_configs AS target
  (task_id, config_id, url, config_proto, local_postgres_task, updated_at)
VALUES ('{POSTGRES_QUERY_PLAN_TASK_ID}', '{POSTGRES_QUERY_PLAN_CONFIG_ID}-insert',
        '{POSTGRES_QUERY_PLAN_URL}', decode('', 'hex'), true, now())
ON CONFLICT (task_id, config_id) DO UPDATE SET url = EXCLUDED.url,
  config_proto = EXCLUDED.config_proto, local_postgres_task = EXCLUDED.local_postgres_task,
  updated_at = now();
SELECT '{POSTGRES_COMBINED_PLAN_START}';
EXPLAIN (ANALYZE, BUFFERS)
WITH task AS MATERIALIZED (
 SELECT 1 FROM a2a_tasks WHERE id = '{POSTGRES_QUERY_PLAN_TASK_ID}'
),
config_count AS MATERIALIZED (
 SELECT count(*) AS total FROM a2a_push_notification_configs
 WHERE task_id = '{POSTGRES_QUERY_PLAN_TASK_ID}'
)
SELECT page.config_proto, config_count.total::text
FROM task CROSS JOIN config_count
LEFT JOIN LATERAL (
 SELECT config_proto FROM a2a_push_notification_configs
 WHERE task_id = '{POSTGRES_QUERY_PLAN_TASK_ID}' AND 0 <= config_count.total
 ORDER BY created_sequence ASC LIMIT ALL OFFSET 0
) AS page ON true;
SELECT '{POSTGRES_COMBINED_PLAN_END}';
ROLLBACK;
'''
    completed = subprocess.run(
        ["psql", dsn, "--no-psqlrc", "--set", "ON_ERROR_STOP=1", "--command", sql],
        check=True, capture_output=True, text=True,
    )
    try:
        combined_plan = completed.stdout.split(POSTGRES_COMBINED_PLAN_START, 1)[1].split(
            POSTGRES_COMBINED_PLAN_END, 1
        )[0]
    except IndexError as error:
        raise ValueError("query-plan review did not capture the combined query plan") from error

    if POSTGRES_PUSH_ORDER_INDEX in combined_plan:
        raise ValueError("query-plan review unexpectedly used the removed push ordering index")
    if "Sort" not in combined_plan or "created_sequence" not in combined_plan:
        raise ValueError("query-plan review did not expose the required created_sequence sort")
    return completed.stdout


def log_progress(message: str) -> None:
    print(f"[perf] {message}", flush=True)


def postgres_tail_expected_rows(config: RunnerConfig) -> int:
    return (len(config.scenarios or ()) * len(config.concurrency_levels) *
            len(config.postgres_pool_sizes) * config.repetitions)


def run_profile_coordinate(config: RunnerConfig, transport: str, concurrency: int,
                           postgres_pool_size: int, schema: str, repetition: int,
                           scenarios: tuple[str, ...]) -> list[dict[str, object]]:
    if config.profile != POSTGRES_WRITE_PROFILE:
        return run_driver(config, transport, "postgres", concurrency, postgres_pool_size, scenarios, schema)
    dsn = os.environ.get("A2A_TEST_POSTGRES_DSN", "")
    if not dsn:
        raise ValueError("A2A_TEST_POSTGRES_DSN must be set for postgres-write")
    collected: list[dict[str, object]] = []
    for scenario_index, scenario in enumerate(scenarios):
        scenario_schema = f"{schema}_s{scenario_index}"
        with PostgresDiagnosticsCollector(dsn) as diagnostics:
            rows = run_driver(
                config, transport, "postgres", concurrency, postgres_pool_size,
                (scenario,), scenario_schema,
            )
        database_diagnostics = diagnostics.result()
        for row in rows:
            row["postgres_database_diagnostics"] = database_diagnostics
            row["repetition"] = repetition
        collected.extend(rows)
    return collected


def log_workload_estimate(config: RunnerConfig) -> None:
    if config.profile in POSTGRES_TAIL_PROFILES:
        estimated_rows = postgres_tail_expected_rows(config)
        log_progress(
            f"profile={config.profile} estimated_rows={estimated_rows} "
            f"estimated_operations={estimated_rows * config.requests}"
        )
        return
    store_pool_count = sum(
        len(config.postgres_pool_sizes) if store_backend == "postgres" else 1
        for store_backend in config.store_backends
    )
    store_concurrency_rows = store_pool_count * len(config.concurrency_levels)
    selected_in_process_scenarios = in_process_scenarios(config.scenarios)
    in_process_rows = store_concurrency_rows * len(selected_in_process_scenarios)
    wire_rows = sum(len(wire_scenarios_for_transport(transport, config.scenarios)) for transport in config.transports) * store_concurrency_rows
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
        last_schema = None
        for store_backend in config.store_backends:
            pool_sizes = config.postgres_pool_sizes if store_backend == "postgres" else (DEFAULT_POSTGRES_POOL_SIZE,)
            for postgres_pool_size in pool_sizes:
                for concurrency in config.concurrency_levels:
                    for repetition in range(1, config.repetitions + 1):
                        schema = None
                        if config.profile in POSTGRES_PROFILES:
                            profile_name = "write" if config.profile == POSTGRES_WRITE_PROFILE else "tail"
                            schema = (f"a2a_{profile_name}_p{postgres_pool_size}_c{concurrency}_"
                                      f"r{repetition}_{os.getpid()}")
                        run_results = run_with_progress(
                            "in-process",
                            lambda schema=schema: (
                                run_profile_coordinate(
                                    config, in_process_transport, concurrency, postgres_pool_size,
                                    schema or "", repetition, config.scenarios or (),
                                ) if config.profile == POSTGRES_WRITE_PROFILE else
                                run_driver(config, in_process_transport, store_backend, concurrency,
                                           postgres_pool_size, in_process_scenarios(config.scenarios), schema)
                            ),
                            in_process_transport, store_backend, concurrency, config.requests,
                        )
                        if config.profile in POSTGRES_PROFILES:
                            for result in run_results:
                                result["repetition"] = repetition
                            last_schema = (f"{schema}_s{len(config.scenarios or ()) - 1}"
                                           if config.profile == POSTGRES_WRITE_PROFILE else schema)
                        results.extend(run_results)
        if config.profile not in POSTGRES_PROFILES:
            for transport in config.transports:
                for store_backend in config.store_backends:
                    pool_sizes = config.postgres_pool_sizes if store_backend == "postgres" else (DEFAULT_POSTGRES_POOL_SIZE,)
                    for postgres_pool_size in pool_sizes:
                        for concurrency in config.concurrency_levels:
                            results.extend(
                                run_with_progress(
                                    "wire",
                                    lambda: run_wire_driver(
                                        config, transport, store_backend, concurrency, postgres_pool_size,
                                        find_available_sut_port()
                                    ),
                                    transport, store_backend, concurrency, config.requests,
                                )
                            )
        results.sort(key=lambda result: (str(result["scenario"]), str(result["store_backend"]), str(result["driver_type"]), str(result["transport_path"]), str(result["transport"]), int(result["concurrency"]), int(result.get("postgres_pool_size") or 0), int(result.get("repetition", 0))))
        aggregates = None
        query_plans = None
        if config.profile in POSTGRES_PROFILES:
            errors = sum(result_error_count(result) for result in results)
            expected_rows = postgres_tail_expected_rows(config)
            if len(results) != expected_rows or errors:
                raise ValueError(
                    f"postgres-tail produced {len(results)}/{expected_rows} rows and {errors} errors"
                )
            aggregates = median_aggregates(results, config.repetitions)
            dsn = os.environ.get("A2A_TEST_POSTGRES_DSN", "")
            if not dsn or last_schema is None:
                raise ValueError("A2A_TEST_POSTGRES_DSN must be set for postgres-tail")
            query_plans = explain_postgres_queries(dsn, last_schema, config.push_config_fanout)
        write_reports(results, config, aggregates, query_plans)
        if any(result_error_count(result) > 0 for result in results):
            print(f"error: {format_error_summary(results)}", file=sys.stderr)
            return 2
        return 0
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
