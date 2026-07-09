#!/usr/bin/env python3
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "scripts" / "run_performance_tests.sh"


class PerformanceRunnerTest(unittest.TestCase):
    def test_writes_reports_for_selected_matrix(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            subprocess.run([
                str(RUNNER),
                "--transports", "grpc",
                "--store-backends", "inmemory",
                "--requests", "3",
                "--concurrency", "1",
                "--warmup-seconds", "0",
                "--report-dir", temp_dir,
            ], cwd=ROOT, check=True)
            report_dir = Path(temp_dir)
            payload = json.loads((report_dir / "results.json").read_text(encoding="utf-8"))
            self.assertEqual(25, len(payload["results"]))
            self.assertEqual({"cpp_sdk_in_process", "wire_tck_sut"}, {result["driver_type"] for result in payload["results"]})
            self.assertIn("wire_grpc", {result["transport_path"] for result in payload["results"]})
            ordered = sorted(payload["results"], key=lambda result: (result["scenario"], result["store_backend"], result["driver_type"], result["transport_path"], result["transport"], result["concurrency"]))
            self.assertEqual(ordered, payload["results"])
            self.assertTrue((report_dir / "results.csv").exists())
            summary = (report_dir / "summary.md").read_text(encoding="utf-8")
            self.assertIn("A2A performance test summary", summary)
            self.assertIn("| Scenario | Rows | Operations | Success | Errors | Avg ops/sec | Worst p95 ms | Worst max ms |", summary)
            self.assertIn("| Scenario | Driver | Path | Transport | Store | Concurrency | Success | Errors | Ops/sec | p50 ms | p95 ms | p99 ms | Max ms |", summary)

    def test_rejects_unknown_transport(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            completed = subprocess.run([
                str(RUNNER),
                "--transports", "websocket",
                "--report-dir", temp_dir,
            ], cwd=ROOT, text=True, capture_output=True, check=False)
            self.assertNotEqual(0, completed.returncode)
            self.assertIn("unsupported selection", completed.stderr)


if __name__ == "__main__":
    unittest.main()
