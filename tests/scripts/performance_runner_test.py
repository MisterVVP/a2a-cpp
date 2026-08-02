#!/usr/bin/env python3
import importlib.util
import json
import os
import socket
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "scripts" / "run_performance_tests.sh"
RUNNER_MODULE = ROOT / "scripts" / "performance_runner.py"


def load_runner_module():
    spec = importlib.util.spec_from_file_location("performance_runner", RUNNER_MODULE)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class PerformanceRunnerTest(unittest.TestCase):
    def test_writes_reports_for_selected_matrix(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            completed = subprocess.run([
                str(RUNNER),
                "--transports", "grpc",
                "--store-backends", "inmemory",
                "--requests", "3",
                "--concurrency", "1",
                "--warmup-seconds", "0",
                "--report-dir", temp_dir,
            ], cwd=ROOT, text=True, capture_output=True, check=True)
            report_dir = Path(temp_dir)
            payload = json.loads((report_dir / "results.json").read_text(encoding="utf-8"))
            self.assertEqual(36, len(payload["results"]))
            self.assertEqual({"cpp_sdk_in_process", "wire_tck_sut"}, {result["driver_type"] for result in payload["results"]})
            wire_rows = [result for result in payload["results"] if result["driver_type"] == "wire_tck_sut"]
            self.assertEqual(14, len(wire_rows))
            self.assertEqual({"wire_grpc"}, {result["transport_path"] for result in wire_rows})
            self.assertEqual({"in_process"}, {result["transport_path"] for result in payload["results"] if result["driver_type"] == "cpp_sdk_in_process"})
            ordered = sorted(payload["results"], key=lambda result: (result["scenario"], result["store_backend"], result["driver_type"], result["transport_path"], result["transport"], result["concurrency"]))
            self.assertEqual(ordered, payload["results"])
            self.assertIn("configured_duration_seconds", payload["results"][0])
            follow_up_rows = [
                result for result in payload["results"]
                if result["scenario"] == "SendMessage_FollowUpExistingTask"
            ]
            self.assertEqual(2, len(follow_up_rows))
            self.assertEqual({1}, {result["history_depth"] for result in follow_up_rows})
            deep_follow_up_rows = [
                result for result in payload["results"]
                if result["scenario"] == "SendMessage_FollowUpAtHistoryDepth/8"
            ]
            self.assertEqual(2, len(deep_follow_up_rows))
            self.assertEqual({8}, {result["history_depth"] for result in deep_follow_up_rows})
            callback_rows = [
                result for result in payload["results"]
                if result["driver_type"] == "cpp_sdk_in_process"
                and result["scenario"] == "PushDelivery_CallbackFanout"
            ]
            self.assertEqual(1, len(callback_rows))
            self.assertEqual(3, callback_rows[0]["success"])
            self.assertEqual(24, callback_rows[0]["successful_deliveries"])
            self.assertEqual(0, callback_rows[0]["failed_deliveries"])
            self.assertEqual(24, callback_rows[0]["callback_count"])
            self.assertEqual(8, callback_rows[0]["fanout_per_operation"])
            self.assertEqual(24, callback_rows[0]["total_fanout_count"])
            self.assertEqual(24, callback_rows[0]["fanout_count"])
            list_many_rows = [
                result for result in payload["results"]
                if result["driver_type"] == "cpp_sdk_in_process"
                and result["scenario"] == "PushConfig_ListManyConfigs"
            ]
            self.assertEqual(1, len(list_many_rows))
            self.assertEqual(8, list_many_rows[0]["fanout_per_operation"])
            self.assertEqual(24, list_many_rows[0]["total_fanout_count"])
            self.assertEqual(24, list_many_rows[0]["fanout_count"])
            self.assertIn("measured_duration_seconds", payload["results"][0])
            streaming_rows = [
                result for result in wire_rows
                if result["scenario"] in {"SendStreamingMessage_FiniteStream", "SubscribeToTask_FirstEventLatency"}
            ]
            self.assertEqual(2, len(streaming_rows))
            self.assertTrue(all(result["event_count"] > 0 for result in streaming_rows))
            self.assertTrue(all("first_event_latency_ms" in result for result in streaming_rows))
            self.assertTrue(all("stream_completion_latency_ms" in result for result in streaming_rows))
            self.assertTrue((report_dir / "results.csv").exists())
            csv_header = (report_dir / "results.csv").read_text(encoding="utf-8").splitlines()[0]
            self.assertIn("first_event_p50_ms", csv_header)
            self.assertIn("fanout_per_operation", csv_header)
            self.assertIn("total_fanout_count", csv_header)
            self.assertIn("fanout_count", csv_header)
            self.assertIn("history_depth", csv_header)
            self.assertIn("stream_completion_p50_ms", csv_header)
            self.assertTrue(any(report_dir.glob("tck_sut_inmemory_*.log")))
            self.assertIn("[perf] estimated_rows=", completed.stdout)
            self.assertIn("[perf] start in-process transport=grpc store=inmemory concurrency=1 requests=3", completed.stdout)
            self.assertIn("[perf] done  wire transport=grpc store=inmemory concurrency=1", completed.stdout)
            summary = (report_dir / "summary.md").read_text(encoding="utf-8")
            self.assertIn("A2A performance test summary", summary)
            self.assertIn("| Store | Path | Rows | Operations | Success | Errors |", summary)
            self.assertIn("## In-process results — In-memory", summary)
            self.assertIn("## Wire results — In-memory — gRPC", summary)
            self.assertNotIn("Repetition", summary)
            self.assertIn("<summary>Show all raw result rows</summary>", summary)

    def test_report_groups_coordinates_and_calculates_cross_backend_scaling(self):
        runner = load_runner_module()
        rows = []
        for store, pool, multiplier in (("inmemory", None, 1.0), ("postgres", 4, 2.0),
                                        ("postgres", 16, 3.0)):
            for path, transport in (("in_process", "grpc"), ("wire_grpc", "grpc"),
                                    ("wire_http_json", "http_json")):
                for concurrency in (2, 8):
                    rows.append(self.make_result(store, pool, path, transport, concurrency,
                                                 multiplier * concurrency,
                                                 multiplier * concurrency))
        metadata = {"sdk_commit_sha": "test", "host": {"os": "test", "cpu": "test"}}
        summary = runner.render_markdown_summary(rows, metadata)
        self.assertIn("## In-process results — PostgreSQL — pool 4", summary)
        self.assertIn("## In-process results — PostgreSQL — pool 16", summary)
        self.assertIn("## Wire results — In-memory — HTTP+JSON", summary)
        self.assertIn("<summary>Show In-process results — In-memory</summary>", summary)
        self.assertIn("<summary>Show Wire results — PostgreSQL — gRPC — pool 4</summary>", summary)
        self.assertIn("<summary>Show cross-backend scaling signals</summary>", summary)
        self.assertIn("| Scenario | c2 ops/s | c2 p95 ms | c8 ops/s | c8 p95 ms |", summary)
        self.assertIn("| scenario | In-process | — | 4 | c2–c8 | 4.00× | 4.00× | 4.00× | 4.00× |", summary)
        self.assertNotIn("| in_process | grpc |", summary)
        self.assertEqual(4, len(runner.execution_rollups(rows)))

    def test_cross_backend_scaling_excludes_unmatched_and_handles_zero(self):
        runner = load_runner_module()
        rows = [
            self.make_result("inmemory", None, "wire_grpc", "grpc", 1, 0.0, 0.0),
            self.make_result("inmemory", None, "wire_grpc", "grpc", 4, 4.0, 4.0),
            self.make_result("postgres", 4, "wire_grpc", "grpc", 1, 0.0, 0.0),
            self.make_result("postgres", 4, "wire_grpc", "grpc", 4, 8.0, 8.0),
            self.make_result("postgres", 4, "wire_jsonrpc", "jsonrpc", 4, 8.0, 8.0),
        ]
        signals = runner.cross_backend_scaling(rows)
        self.assertEqual(1, len(signals))
        self.assertIsNone(signals[0]["memory_p95_growth"])
        self.assertIsNone(signals[0]["postgres_throughput_scaling"])

    def test_cross_backend_scaling_requires_two_shared_concurrency_levels(self):
        runner = load_runner_module()
        rows = [
            self.make_result("inmemory", None, "wire_grpc", "grpc", 1, 10.0, 1.0),
            self.make_result("postgres", 4, "wire_grpc", "grpc", 1, 5.0, 2.0),
        ]

        self.assertEqual([], runner.cross_backend_scaling(rows))
        metadata = {"sdk_commit_sha": "test", "host": {"os": "test", "cpu": "test"}}
        summary = runner.render_markdown_summary(rows, metadata)
        self.assertIn("| _No comparable coordinates_ |", summary)
        self.assertNotIn("c1–c1", summary)

    def test_detailed_matrix_conditionally_includes_repetition(self):
        runner = load_runner_module()
        row = self.make_result("inmemory", None, "in_process", "grpc", 1, 1.0, 1.0)
        lines = []
        runner.append_detailed_matrix(lines, [row])
        self.assertNotIn("Repetition", "\n".join(lines))
        row["repetition"] = 1
        lines = []
        runner.append_detailed_matrix(lines, [row])
        self.assertIn("Repetition", "\n".join(lines))
        self.assertIn("| — | In-memory |", "\n".join(lines))

    def test_postgres_tail_large_tables_are_collapsible(self):
        runner = load_runner_module()
        rows = self.make_postgres_tail_rows(runner)
        aggregates = runner.median_aggregates(rows, runner.POSTGRES_TAIL_REPETITIONS)
        metadata = {"sdk_commit_sha": "test", "host": {"os": "test", "cpu": "test"}}
        summary = runner.render_markdown_summary(rows, metadata, aggregates)
        self.assertIn("<summary>Show PostgreSQL phase diagnostics</summary>", summary)
        self.assertIn("<summary>Show median aggregates</summary>", summary)
        self.assertIn("<summary>Show median PostgreSQL phases</summary>", summary)

    @staticmethod
    def make_result(store, pool, path, transport, concurrency, throughput, p95):
        return {
            "scenario": "scenario", "transport": transport, "store_backend": store,
            "driver_type": "cpp_sdk_in_process" if path == "in_process" else "wire_tck_sut",
            "transport_path": path, "concurrency": concurrency, "postgres_pool_size": pool,
            "operations": 10, "success": 10, "errors": 0,
            "throughput_ops_per_sec": throughput,
            "latency_ms": {"p50": p95 / 2, "p90": p95, "p95": p95,
                           "p99": p95, "max": p95},
        }

    def test_wire_scenarios_include_streaming_for_http_transports(self):
        runner = load_runner_module()
        self.assertIn("SendStreamingMessage_FiniteStream", runner.wire_scenarios_for_transport("grpc"))
        self.assertIn("SendStreamingMessage_FiniteStream", runner.wire_scenarios_for_transport("jsonrpc"))
        self.assertIn("SubscribeToTask_FirstEventLatency", runner.wire_scenarios_for_transport("http_json"))
        self.assertIn("PushConfig_Create", runner.wire_scenarios_for_transport("jsonrpc"))
        self.assertIn("PushConfig_Delete", runner.wire_scenarios_for_transport("http_json"))

    def test_rejects_unknown_transport(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            completed = subprocess.run([
                str(RUNNER),
                "--transports", "websocket",
                "--report-dir", temp_dir,
            ], cwd=ROOT, text=True, capture_output=True, check=False)
            self.assertNotEqual(0, completed.returncode)
            self.assertIn("unsupported selection", completed.stderr)

    def test_rejects_driver_timeout_clearly(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            sleeper = Path(temp_dir) / "sleepy_driver.py"
            sleeper.write_text("#!/usr/bin/env python3\nimport time\ntime.sleep(10)\n", encoding="utf-8")
            sleeper.chmod(0o755)
            env = os.environ.copy()
            env["A2A_PERF_DRIVER"] = str(sleeper)
            completed = subprocess.run([
                str(RUNNER),
                "--transports", "grpc",
                "--store-backends", "inmemory",
                "--requests", "1",
                "--concurrency", "1",
                "--warmup-seconds", "0",
                "--driver-timeout-seconds", "0.1",
                "--report-dir", temp_dir,
            ], cwd=ROOT, env=env, text=True, capture_output=True, check=False)
            self.assertNotEqual(0, completed.returncode)
            self.assertIn("timed out", completed.stderr)

    def test_formats_operation_error_summary(self):
        runner = load_runner_module()
        summary = runner.format_error_summary([
            {
                "scenario": "SendMessage_CreateTask",
                "driver_type": "wire_tck_sut",
                "transport_path": "wire_jsonrpc",
                "transport": "jsonrpc",
                "store_backend": "inmemory",
                "concurrency": 4,
                "success": 3,
                "errors": 2,
            }
        ])
        self.assertIn("2 operation errors", summary)
        self.assertIn("scenario=SendMessage_CreateTask", summary)
        self.assertIn("transport_path=wire_jsonrpc", summary)
        self.assertIn("concurrency=4", summary)

    def test_find_available_sut_port_returns_adjacent_bindable_ports(self):
        runner = load_runner_module()
        port = runner.find_available_sut_port()
        self.assertGreater(port, 0)
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as http_probe:
            http_probe.bind(("127.0.0.1", port))
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as grpc_probe:
            grpc_probe.bind(("127.0.0.1", port + 1))

    def test_postgres_schema_name_is_matrix_scoped(self):
        runner = load_runner_module()
        self.assertEqual("a2a_perf_http_json_4_51081", runner.postgres_schema_name("http_json", 4, 51081))

    def test_postgres_tail_profile_has_larger_default_validation_matrix(self):
        runner = load_runner_module()
        with mock.patch.dict(os.environ, {}, clear=True):
            config = runner.parse_args(["--profile", "postgres-tail"])
        self.assertEqual(("grpc",), config.transports)
        self.assertEqual(("postgres",), config.store_backends)
        self.assertEqual(2_000, config.requests)
        self.assertEqual((4, 16, 64), config.concurrency_levels)
        self.assertEqual((4, 16, 64), config.postgres_pool_sizes)
        self.assertEqual(5, config.repetitions)
        self.assertEqual(1.0, config.warmup_seconds)
        self.assertEqual(runner.POSTGRES_TAIL_SCENARIOS, config.scenarios)
        self.assertEqual(225, runner.postgres_tail_expected_rows(config))

    def test_postgres_tail_c1_profile_is_focused(self):
        runner = load_runner_module()
        with mock.patch.dict(os.environ, {}, clear=True):
            config = runner.parse_args(["--profile", "postgres-tail-c1"])
        self.assertEqual(("grpc",), config.transports)
        self.assertEqual(("postgres",), config.store_backends)
        self.assertEqual(2_000, config.requests)
        self.assertEqual((1,), config.concurrency_levels)
        self.assertEqual((64,), config.postgres_pool_sizes)
        self.assertEqual(5, config.repetitions)
        self.assertEqual(runner.POSTGRES_TAIL_C1_SCENARIOS, config.scenarios)
        self.assertEqual(5, runner.postgres_tail_expected_rows(config))

    def test_postgres_tail_profile_allows_pool_size_override(self):
        runner = load_runner_module()
        with mock.patch.dict(os.environ, {"A2A_PERF_POSTGRES_POOL_SIZES": "8,32"}, clear=True):
            config = runner.parse_args(["--profile", "postgres-tail"])
            cli_config = runner.parse_args([
                "--profile", "postgres-tail", "--postgres-pool-sizes", "16,64",
            ])
        self.assertEqual((8, 32), config.postgres_pool_sizes)
        self.assertEqual((16, 64), cli_config.postgres_pool_sizes)
        self.assertEqual(150, runner.postgres_tail_expected_rows(config))
        self.assertEqual(150, runner.postgres_tail_expected_rows(cli_config))

    def test_rejects_duplicate_postgres_pool_sizes(self):
        runner = load_runner_module()
        with mock.patch.dict(os.environ, {}, clear=True):
            with self.assertRaisesRegex(ValueError, "PostgreSQL pool sizes must not contain duplicates"):
                runner.parse_args(["--postgres-pool-sizes", "4,4"])

    def test_normal_workload_estimate_counts_postgres_pool_sizes(self):
        runner = load_runner_module()
        with mock.patch.dict(os.environ, {}, clear=True):
            config = runner.parse_args([
                "--transports", "grpc",
                "--store-backends", "inmemory,postgres",
                "--requests", "10",
                "--concurrency", "1,4",
                "--postgres-pool-sizes", "4,16,64",
            ])

        non_postgres_stores = [
            store_backend for store_backend in config.store_backends
            if store_backend != "postgres"
        ]
        store_pool_count = len(config.postgres_pool_sizes) + len(non_postgres_stores)
        matrix_entries = store_pool_count * len(config.concurrency_levels)
        expected_rows = matrix_entries * (
            len(runner.SCENARIOS) + len(runner.wire_scenarios_for_transport("grpc"))
        )
        expected_message = (
            f"estimated_rows={expected_rows} "
            f"estimated_operations={expected_rows * config.requests} "
            "transports=1 stores=2 concurrency_levels=2 requests=10"
        )

        with mock.patch.object(runner, "log_progress") as log_progress:
            runner.log_workload_estimate(config)

        log_progress.assert_called_once_with(expected_message)

    def test_postgres_tail_aggregates_and_reports(self):
        runner = load_runner_module()
        rows = self.make_postgres_tail_rows(runner)
        aggregates = runner.median_aggregates(rows, runner.POSTGRES_TAIL_REPETITIONS)
        self.assertEqual((4, 16, 64), runner.POSTGRES_TAIL_CONCURRENCY)
        self.assertEqual(45, len(aggregates))
        self.assertEqual(5, aggregates[0]["repetitions"])
        self.assertEqual(103.0, aggregates[0]["throughput_ops_per_sec"])
        self.assertEqual(3.0, aggregates[0]["p95_ms"])
        phase = runner.POSTGRES_DIAGNOSTIC_PHASES[0]
        self.assertEqual(4.0, aggregates[0]["postgres_phase_latency_ms"][phase]["p99"])
        config = runner.parse_args(["--profile", "postgres-tail"])
        with tempfile.TemporaryDirectory() as directory:
            config = runner.RunnerConfig(**{**config.__dict__, "report_dir": Path(directory)})
            runner.write_reports(rows, config, aggregates, "Index Scan using a2a_tasks_pkey")
            payload = json.loads((Path(directory) / "results.json").read_text(encoding="utf-8"))
            summary = (Path(directory) / "summary.md").read_text(encoding="utf-8")
            aggregate_csv = (Path(directory) / "median-aggregates.csv").read_text(encoding="utf-8")
        self.assertEqual(225, len(payload["results"]))
        self.assertEqual(45, len(payload["median_aggregates"]))
        self.assertIn("Median PostgreSQL phases", summary)
        self.assertIn("Query-plan review", summary)
        self.assertIn("connection_acquire_wait_p99_ms", aggregate_csv)
        self.assertNotIn("executor_lock_wait", aggregate_csv)

    def test_postgres_tail_rejects_incomplete_aggregate(self):
        runner = load_runner_module()
        with self.assertRaisesRegex(ValueError, "4/5 repetitions"):
            runner.median_aggregates(self.make_postgres_tail_rows(runner)[:-1], 5)

    def test_query_plan_requires_task_and_push_indexes(self):
        runner = load_runner_module()
        completed = subprocess.CompletedProcess(
            args=[], returncode=0,
            stdout=("Index Scan using a2a_tasks_pkey\n"
                    "Index Scan using idx_a2a_push_configs_task\n"
                    "Index Scan using idx_a2a_push_configs_created_sequence"),
            stderr="",
        )
        with mock.patch.object(runner.subprocess, "run", return_value=completed) as run:
            plans = runner.explain_postgres_queries("postgresql://test", "schema")
        self.assertIn("idx_a2a_push_configs_created_sequence", plans)
        command = run.call_args.args[0]
        sql = command[-1]
        self.assertIn("WITH task AS MATERIALIZED", sql)
        self.assertIn("config_count AS MATERIALIZED", sql)
        self.assertIn("LEFT JOIN LATERAL", sql)
        self.assertIn("AND 0 <= config_count.total", sql)
        self.assertIn("ORDER BY created_sequence ASC LIMIT 1 OFFSET 0", sql)
        self.assertIn("WHERE task_id = ''", sql)
        self.assertIn("SET enable_sort = off", sql)
        self.assertNotIn("SELECT task_id FROM", sql)
        completed.stdout = ("Index Scan using a2a_tasks_pkey\n"
                            "Index Scan using idx_a2a_push_configs_created_sequence")
        with mock.patch.object(runner.subprocess, "run", return_value=completed):
            with self.assertRaisesRegex(ValueError, "idx_a2a_push_configs_task"):
                runner.explain_postgres_queries("postgresql://test", "schema")

        for missing_index in ("a2a_tasks_pkey", "idx_a2a_push_configs_created_sequence"):
            completed.stdout = "\n".join(index for index in (
                "a2a_tasks_pkey", "idx_a2a_push_configs_task", "idx_a2a_push_configs_created_sequence"
            ) if index != missing_index)
            with mock.patch.object(runner.subprocess, "run", return_value=completed):
                with self.assertRaisesRegex(ValueError, missing_index):
                    runner.explain_postgres_queries("postgresql://test", "schema")

    @staticmethod
    def make_postgres_tail_rows(runner):
        rows = []
        for pool_size in runner.POSTGRES_TAIL_POOL_SIZES:
            for concurrency in runner.POSTGRES_TAIL_CONCURRENCY:
                for scenario in runner.POSTGRES_TAIL_SCENARIOS:
                    for repetition in range(1, runner.POSTGRES_TAIL_REPETITIONS + 1):
                        phases = {
                            phase: {"p95": float(repetition), "p99": float(repetition + 1),
                                    "max": float(repetition + 2)}
                            for phase in runner.POSTGRES_DIAGNOSTIC_PHASES
                        }
                        rows.append({
                            "repetition": repetition, "scenario": scenario, "transport": "grpc",
                            "store_backend": "postgres", "driver_type": "cpp_sdk_in_process",
                            "transport_path": "in_process", "concurrency": concurrency,
                            "postgres_pool_size": pool_size,
                            "operations": runner.DEFAULT_REQUESTS, "success": runner.DEFAULT_REQUESTS,
                            "errors": 0, "throughput_ops_per_sec": float(100 + repetition),
                            "latency_ms": {"p50": 1.0, "p90": 2.0, "p95": float(repetition),
                                           "p99": float(repetition + 1), "max": float(repetition + 2)},
                            "postgres_phase_latency_ms": phases,
                        })
        return rows


if __name__ == "__main__":
    unittest.main()
