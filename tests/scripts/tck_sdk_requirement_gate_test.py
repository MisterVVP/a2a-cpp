#!/usr/bin/env python3
"""Regression checks for the mandatory SDK requirement report gate."""

import copy
import importlib.util
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("sdk_gate", ROOT / "scripts/check_tck_sdk_requirements.py")
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)


class RequirementGateTest(unittest.TestCase):
    def setUp(self):
        self.report = {"per_requirement": {
            requirement: {"status": "PASS", "transports": {binding: "PASS" for binding in GATE.TRANSPORTS},
                          "test_ids": [f"test_cpp_sdk_requirements.py::test_cpp_sdk_requirement[{requirement}-{binding}]"
                                       for binding in GATE.TRANSPORTS]}
            for requirement in GATE.REQUIREMENTS
        }}

    def test_all_nine_requirements_need_all_three_bindings(self):
        self.assertEqual(GATE.check_report(self.report), [])
        for requirement in GATE.REQUIREMENTS:
            for binding in GATE.TRANSPORTS:
                with self.subTest(requirement=requirement, binding=binding):
                    report = copy.deepcopy(self.report)
                    del report["per_requirement"][requirement]["transports"][binding]
                    self.assertTrue(GATE.check_report(report))

    def test_skip_untested_failure_and_missing_requirements_fail(self):
        for requirement in GATE.REQUIREMENTS:
            for status in ("SKIPPED", "NOT TESTED", "FAIL"):
                with self.subTest(requirement=requirement, status=status):
                    report = copy.deepcopy(self.report)
                    report["per_requirement"][requirement]["status"] = status
                    self.assertTrue(GATE.check_report(report))
            report = copy.deepcopy(self.report)
            del report["per_requirement"][requirement]
            self.assertTrue(GATE.check_report(report))

    def test_pass_aggregate_cannot_hide_failed_or_skipped_binding(self):
        for status in ("FAIL", "SKIPPED", "NOT TESTED"):
            report = copy.deepcopy(self.report)
            report["per_requirement"][GATE.REQUIREMENTS[0]]["transports"]["grpc"] = status
            self.assertTrue(GATE.check_report(report))

    def test_upstream_only_results_do_not_replace_sdk_client_evidence(self):
        for requirement in GATE.REQUIREMENTS:
            report = copy.deepcopy(self.report)
            report["per_requirement"][requirement]["test_ids"] = ["test_error_handling.py::test_version_error"]
            self.assertTrue(GATE.check_report(report))

    def test_sdk_evidence_is_required_for_each_binding(self):
        for requirement in GATE.REQUIREMENTS:
            report = copy.deepcopy(self.report)
            report["per_requirement"][requirement]["test_ids"].pop()
            self.assertTrue(GATE.check_report(report))


if __name__ == "__main__":
    unittest.main()
