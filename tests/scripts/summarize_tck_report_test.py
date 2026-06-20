#!/usr/bin/env python3
"""Tests for the TCK compatibility report summarizer."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "summarize_tck_report.py"
CORE_CAPABILITY_REQUIREMENT = "CORE-CAP-001"
CARD_SIGNING_REQUIREMENT = "CARD-SIGN-001"
STREAM_SUBSCRIPTION_REQUIREMENT = "STREAM-SUB-002"
AGGREGATE_SKIPPED_LABEL = "aggregate-skipped"
AGGREGATE_FAILED_LABEL = "aggregate-failed"
JSONRPC_TRANSPORT = "jsonrpc"
HTTP_JSON_TRANSPORT = "http_json"
GRPC_TRANSPORT = "grpc"
PASS_STATUS = "PASS"
SKIPPED_STATUS = "SKIPPED"
FAILED_STATUS = "FAIL"
NOT_TESTED_STATUS = "NOT TESTED"
TERMINAL_ERROR = "terminal event missing"
JUNIT_FILENAME = "junitreport.xml"
COMPATIBILITY_FILENAME = "compatibility.json"


class SummarizeTckReportTest(unittest.TestCase):
    def test_per_requirement_map_keys_are_reported_before_aggregates(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            compatibility_path = Path(tmpdir) / COMPATIBILITY_FILENAME
            junit_path = Path(tmpdir) / JUNIT_FILENAME
            compatibility_path.write_text(json.dumps(self._compatibility_report()), encoding="utf-8")
            junit_path.write_text(self._junit_report(), encoding="utf-8")

            completed = subprocess.run(
                [sys.executable, str(SCRIPT), str(compatibility_path), "--require-zero-gaps"],
                check=False,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        output = completed.stdout + completed.stderr
        self.assertEqual(completed.returncode, 1)
        self.assertIn(CORE_CAPABILITY_REQUIREMENT, output)
        self.assertIn(CARD_SIGNING_REQUIREMENT, output)
        self.assertIn(STREAM_SUBSCRIPTION_REQUIREMENT, output)
        self.assertIn(TERMINAL_ERROR, output)
        self.assertIn("Skipped test cases (1)", output)
        self.assertNotIn(AGGREGATE_SKIPPED_LABEL, output)

    def test_empty_per_requirement_map_falls_back_to_aggregates(self) -> None:
        report = {
            "overall": {"passed": 9, "failed": 1, "total": 10},
            "per_requirement": {},
        }
        with tempfile.TemporaryDirectory() as tmpdir:
            compatibility_path = Path(tmpdir) / COMPATIBILITY_FILENAME
            compatibility_path.write_text(json.dumps(report), encoding="utf-8")

            completed = subprocess.run(
                [sys.executable, str(SCRIPT), str(compatibility_path), "--require-zero-gaps"],
                check=False,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        output = completed.stdout + completed.stderr
        self.assertEqual(completed.returncode, 1)
        self.assertIn(AGGREGATE_FAILED_LABEL, output)

    @staticmethod
    def _compatibility_report() -> dict[str, object]:
        return {
            "overall": {"passed": 10, "skipped": 3, "total": 20},
            "per_transport": {GRPC_TRANSPORT: {"passed": 1, "skipped": 4, "total": 5}},
            "per_requirement": {
                CORE_CAPABILITY_REQUIREMENT: {
                    "status": SKIPPED_STATUS,
                    "transports": {
                        JSONRPC_TRANSPORT: {"status": SKIPPED_STATUS},
                        HTTP_JSON_TRANSPORT: {"status": SKIPPED_STATUS},
                        GRPC_TRANSPORT: {"status": PASS_STATUS},
                    },
                },
                CARD_SIGNING_REQUIREMENT: {"status": NOT_TESTED_STATUS},
                STREAM_SUBSCRIPTION_REQUIREMENT: {
                    "status": FAILED_STATUS,
                    "transports": [JSONRPC_TRANSPORT],
                    "errors": [TERMINAL_ERROR],
                },
            },
        }

    @staticmethod
    def _junit_report() -> str:
        return f"""<testsuite>
  <testcase classname="tests.compatibility.test_capabilities" name="test_capability[jsonrpc]">
    <skipped message="{CORE_CAPABILITY_REQUIREMENT} disabled for this capability profile" />
  </testcase>
</testsuite>
"""


if __name__ == "__main__":
    unittest.main()
