#!/usr/bin/env python3
"""Tests for the focused PostgreSQL tail validation report generator."""

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "postgres_tail_validation.py"
SPEC = importlib.util.spec_from_file_location("postgres_tail_validation", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class PostgresTailValidationTest(unittest.TestCase):
    def make_rows(self) -> list[dict[str, object]]:
        rows: list[dict[str, object]] = []
        for concurrency in MODULE.CONCURRENCY_LEVELS:
            for scenario in MODULE.SCENARIOS:
                for repetition in range(1, MODULE.REPETITIONS + 1):
                    phases = {
                        phase: {"p50": 0.1, "p95": float(repetition), "p99": float(repetition + 1),
                                "max": float(repetition + 2)}
                        for phase in MODULE.PHASES
                    }
                    rows.append({"repetition": repetition, "scenario": scenario, "concurrency": concurrency,
                                 "operations": MODULE.REQUESTS, "errors": 0,
                                 "throughput_ops_per_sec": float(100 + repetition),
                                 "latency_ms": {"p95": float(repetition), "p99": float(repetition + 1),
                                                "max": float(repetition + 2)},
                                 "postgres_phase_latency_ms": phases})
        return rows

    def test_median_aggregates_include_all_scenarios_and_phases(self) -> None:
        aggregates = MODULE.median_aggregates(self.make_rows())
        self.assertEqual(len(aggregates), len(MODULE.SCENARIOS) * len(MODULE.CONCURRENCY_LEVELS))
        first = aggregates[0]
        self.assertEqual(first["repetitions"], MODULE.REPETITIONS)
        self.assertEqual(first["throughput_ops_per_sec"], 103.0)
        self.assertEqual(first["p95_ms"], 3.0)
        self.assertEqual(first["postgres_phase_latency_ms"][MODULE.PHASES[0]]["p99"], 4.0)

    def test_reports_contain_repetitions_aggregates_and_no_obsolete_lock_phases(self) -> None:
        rows = self.make_rows()
        aggregates = MODULE.median_aggregates(rows)
        with tempfile.TemporaryDirectory() as directory:
            report_dir = Path(directory)
            MODULE.write_reports(report_dir, rows, aggregates, "Index Scan using a2a_tasks_pkey")
            payload = json.loads((report_dir / "results.json").read_text(encoding="utf-8"))
            summary = (report_dir / "summary.md").read_text(encoding="utf-8")
            csv_report = (report_dir / "results.csv").read_text(encoding="utf-8")
        self.assertEqual(len(payload["results"]), len(rows))
        self.assertEqual(len(payload["median_aggregates"]), len(aggregates))
        self.assertIn("Median PostgreSQL phases", summary)
        self.assertIn("Repetition PostgreSQL phases", summary)
        self.assertNotIn("executor_lock_wait", csv_report)
        self.assertNotIn("executor_lock_hold", summary)


if __name__ == "__main__":
    unittest.main()
