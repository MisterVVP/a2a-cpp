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
            self.assertEqual(18, len(payload["results"]))
            self.assertTrue((report_dir / "results.csv").exists())
            self.assertIn("A2A performance test summary", (report_dir / "summary.md").read_text(encoding="utf-8"))

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
