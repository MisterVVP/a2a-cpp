#!/usr/bin/env python3
"""Regression gate must reject incomplete, failed and slower reactor evidence."""

import copy
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "compare_curl_reactor_evidence.py"
SPEC = importlib.util.spec_from_file_location("reactor_evidence", SCRIPT)
EVIDENCE = importlib.util.module_from_spec(SPEC)
sys.dont_write_bytecode = True
SPEC.loader.exec_module(EVIDENCE)


def rows():
    return [dict(streams=count, mode=mode, samples=count * 20, errors=0, timer_period_ms=1,
                 maximum_thread_growth=8, first_p95_ms=1.0, finish_p95_ms=1.0, finish_p99_ms=2.0)
            for count in EVIDENCE.STREAM_COUNTS for mode in EVIDENCE.MODES]


class EvidenceTest(unittest.TestCase):
    def read(self, values):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "evidence.json"
            path.write_text(json.dumps(values), encoding="utf-8")
            return EVIDENCE.read_evidence(path)

    def test_equivalent_workloads_pass(self):
        indexed = self.read(rows())
        report, failures = EVIDENCE.compare(indexed, indexed)
        self.assertFalse(failures)
        self.assertIn("96", report)

    def test_missing_duplicate_failed_and_invalid_evidence_rejected(self):
        valid = rows()
        for invalid in (valid[:-1], valid + [valid[0]]):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                self.read(invalid)
        for field, value in (("errors", 1), ("samples", 0), ("maximum_thread_growth", 96),
                             ("finish_p95_ms", -1), ("finish_p99_ms", float("nan")), ("timer_period_ms", -1)):
            invalid = copy.deepcopy(valid)
            invalid[0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.read(invalid)

    def test_latency_regression_fails(self):
        baseline = self.read(rows())
        slower = copy.deepcopy(baseline)
        slower[(64, "complete")]["first_p95_ms"] = 10
        _, failures = EVIDENCE.compare(baseline, slower)
        self.assertTrue(failures)

    def test_cancellation_targets_enforced_even_with_slow_baseline(self):
        baseline = self.read(rows())
        baseline[(64, "cancel")].update(finish_p95_ms=10, finish_p99_ms=20)
        _, failures = EVIDENCE.compare(baseline, baseline)
        self.assertTrue(failures)

    def test_unequal_sample_counts_fail(self):
        baseline = self.read(rows())
        candidate = copy.deepcopy(baseline)
        candidate[(16, "complete")]["samples"] += 1
        _, failures = EVIDENCE.compare(baseline, candidate)
        self.assertTrue(failures)

    def test_unequal_timer_periods_fail(self):
        baseline = self.read(rows())
        candidate = copy.deepcopy(baseline)
        candidate[(16, "complete")]["timer_period_ms"] = 0
        _, failures = EVIDENCE.compare(baseline, candidate)
        self.assertTrue(any("unequal timer periods" in failure for failure in failures))


if __name__ == "__main__":
    unittest.main()
