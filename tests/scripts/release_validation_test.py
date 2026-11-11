# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)
"""Exercise evidence collection failure handling without building the SDK."""
import contextlib
import json
from pathlib import Path
import platform
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "performance"))
import release_validation as runner


@unittest.skipUnless(platform.system() == "Linux", "Linux /proc resource sampling")
class ReleaseValidationTest(unittest.TestCase):
    def test_listener_pair_avoids_client_ephemeral_range(self):
        port = runner.listener_ports()
        start = int(Path("/proc/sys/net/ipv4/ip_local_port_range").read_text().split()[0])
        self.assertGreaterEqual(port, 1024)
        self.assertLess(port + 1, start)

    def test_child_exit_keeps_raw_results_and_resource_samples(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            row, metrics = runner.run_driver(
                [sys.executable, "-c", "import time; time.sleep(0.05); print('[{\"errors\": 0}]')"], root, "success")
            self.assertEqual(row, [{"errors": 0}])
            self.assertGreater(metrics["peak_rss_kib"], 0)
            self.assertGreaterEqual(metrics["peak_threads"], 1)
            self.assertEqual(json.loads((root / "success-driver.json").read_text()), row)

    def test_driver_failure_preserves_stdout_and_fails_gate(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            with self.assertRaisesRegex(RuntimeError, "Driver exited with 3"):
                runner.run_driver([sys.executable, "-c", "import time; time.sleep(0.05); print('evidence'); exit(3)"], root, "failed")
            self.assertEqual((root / "failed-driver.json").read_text().strip(), "evidence")

    def test_timeout_fails_gate(self):
        with tempfile.TemporaryDirectory() as scratch, patch.object(runner, "DRIVER_TIMEOUT", 0.05):
            with self.assertRaisesRegex(RuntimeError, "Driver timed out"):
                runner.run_driver([sys.executable, "-c", "import time; time.sleep(5)"], Path(scratch), "timeout")

    def test_wire_rejects_errors_or_wrong_sample_count(self):
        process = type("Process", (), {"pid": 1})()
        for row in ({"operations": 64, "errors": 1}, {"operations": 63, "errors": 0}):
            with self.subTest(row=row), \
                    patch.object(runner, "server", return_value=contextlib.nullcontext((process, 1))), \
                    patch.object(runner, "monitor", return_value=contextlib.nullcontext({})), \
                    patch.object(runner, "run_driver", return_value=([row], {})):
                with self.assertRaisesRegex(RuntimeError, "zero-error row"):
                    runner.wire_case(Path("sut"), Path("driver"), "inmemory", "grpc", 1, 64, "unary", Path("."), "case")

    def test_comparison_uses_repetition_medians_and_sync_baseline(self):
        rows = []
        for mode, rate in (("baseline_sync", 100), ("candidate_sync", 80), ("candidate_async", 200)):
            for factor in (1, 1, 10):
                rows.append({"runtime": mode, "store_backend": "inmemory", "transport": "grpc",
                             "scenario": runner.SCENARIOS[0], "concurrency": 1,
                             "throughput_ops_per_sec": rate * factor, "latency_ms": {"p95": 1}})
        self.assertIn("100.0 | 80.0 (-20.0%) | 200.0", runner.comparison(rows))


if __name__ == "__main__":
    unittest.main()
