#!/usr/bin/env python3
import importlib.util
import json
import os
import socket
import subprocess
import sys
import tempfile
import unittest
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
            self.assertEqual(34, len(payload["results"]))
            self.assertEqual({"cpp_sdk_in_process", "wire_tck_sut"}, {result["driver_type"] for result in payload["results"]})
            wire_rows = [result for result in payload["results"] if result["driver_type"] == "wire_tck_sut"]
            self.assertEqual(13, len(wire_rows))
            self.assertEqual({"wire_grpc"}, {result["transport_path"] for result in wire_rows})
            self.assertEqual({"in_process"}, {result["transport_path"] for result in payload["results"] if result["driver_type"] == "cpp_sdk_in_process"})
            ordered = sorted(payload["results"], key=lambda result: (result["scenario"], result["store_backend"], result["driver_type"], result["transport_path"], result["transport"], result["concurrency"]))
            self.assertEqual(ordered, payload["results"])
            self.assertIn("configured_duration_seconds", payload["results"][0])
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
            self.assertEqual(24, callback_rows[0]["fanout_count"])
            list_many_rows = [
                result for result in payload["results"]
                if result["driver_type"] == "cpp_sdk_in_process"
                and result["scenario"] == "PushConfig_ListManyConfigs"
            ]
            self.assertEqual(1, len(list_many_rows))
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
            self.assertIn("fanout_count", csv_header)
            self.assertIn("stream_completion_p50_ms", csv_header)
            self.assertTrue(any(report_dir.glob("tck_sut_inmemory_*.log")))
            self.assertIn("[perf] estimated_rows=", completed.stdout)
            self.assertIn("[perf] start in-process transport=grpc store=inmemory concurrency=1 requests=3", completed.stdout)
            self.assertIn("[perf] done  wire transport=grpc store=inmemory concurrency=1", completed.stdout)
            summary = (report_dir / "summary.md").read_text(encoding="utf-8")
            self.assertIn("A2A performance test summary", summary)
            self.assertIn("| Scenario | Rows | Operations | Success | Errors | Avg ops/sec | Worst p95 ms | Worst max ms |", summary)
            self.assertIn("| Scenario | Driver | Path | Transport | Store | Concurrency | Success | Errors | Ops/sec | p50 ms | p95 ms | p99 ms | Max ms |", summary)

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


if __name__ == "__main__":
    unittest.main()
