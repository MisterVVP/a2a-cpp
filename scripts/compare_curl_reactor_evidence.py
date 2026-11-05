#!/usr/bin/env python3
"""Validate equivalent fallback/native reactor workloads and report tail latency."""

import argparse
import json
import math
from pathlib import Path

STREAM_COUNTS = (1, 16, 64, 96)
MODES = ("complete", "cancel")
MAX_THREAD_GROWTH = 8
LATENCY_PROFILES = {
    "strict": dict(regression_ratio=1.25, scheduling_tolerance_ms=2.0,
                   cancel_p95_ms=10.0, cancel_p99_ms=20.0),
    "cloud": dict(regression_ratio=2.5, scheduling_tolerance_ms=20.0,
                  cancel_p95_ms=50.0, cancel_p99_ms=100.0),
}
METRICS = ("first_p95_ms", "finish_p95_ms", "finish_p99_ms")


def read_evidence(path):
    rows = json.loads(Path(path).read_text(encoding="utf-8-sig"))
    indexed = {}
    for row in rows:
        key = (row["streams"], row["mode"])
        if key in indexed:
            raise ValueError(f"duplicate workload: {key}")
        if row["errors"] != 0 or row["samples"] < row["streams"]:
            raise ValueError(f"failed or incomplete workload: {key}")
        if not 0 <= row["maximum_thread_growth"] <= MAX_THREAD_GROWTH:
            raise ValueError(f"thread budget exceeded: {key}")
        if any(not math.isfinite(row[name]) or row[name] < 0 for name in METRICS):
            raise ValueError(f"invalid latency: {key}")
        if row["finish_p95_ms"] > row["finish_p99_ms"]:
            raise ValueError(f"invalid percentiles: {key}")
        if row.get("timer_period_ms", 0) not in (0, 1):
            raise ValueError(f"invalid timer period: {key}")
        indexed[key] = row
    expected = {(count, mode) for count in STREAM_COUNTS for mode in MODES}
    if set(indexed) != expected:
        raise ValueError("missing or unexpected workloads")
    return indexed


def compare(baseline, candidate, profile="strict"):
    budgets = LATENCY_PROFILES[profile]
    failures = []
    lines = [
        "# Windows libcurl reactor evidence",
        "",
        "All streams established before cancellation/finish. "
        "Cancellation includes dispatch completion. Zero errors and at most eight added SDK threads required.",
        f"Latency profile: {profile}. Cancellation targets: p95 < {budgets['cancel_p95_ms']:g} ms / "
        f"p99 < {budgets['cancel_p99_ms']:g} ms.",
        "Timer periods are requested by the evidence executable (0 means system default). "
        "The SDK does not change timer resolution.",
        "",
        "| Streams | Mode | First p95, fallback/native ms | Finish p95, fallback/native ms | "
        "Finish p99, fallback/native ms | Added threads, fallback/native | Samples, fallback/native | "
        "Timer period, fallback/native ms |",
        "| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for key, native in candidate.items():
        old = baseline[key]
        if old["samples"] != native["samples"]:
            failures.append(f"{key}: unequal sample counts")
        if old.get("timer_period_ms", 0) != native.get("timer_period_ms", 0):
            failures.append(f"{key}: unequal timer periods")
        for metric in METRICS:
            budget = max(old[metric] * budgets["regression_ratio"],
                         old[metric] + budgets["scheduling_tolerance_ms"])
            if native[metric] > budget:
                failures.append(f"{key}: {metric} {native[metric]:.3f} ms exceeds {budget:.3f} ms")
        if key[1] == "cancel":
            if (native["finish_p95_ms"] >= budgets["cancel_p95_ms"] or
                    native["finish_p99_ms"] >= budgets["cancel_p99_ms"]):
                failures.append(f"{key}: cancellation target p95 < {budgets['cancel_p95_ms']:g} ms / "
                                f"p99 < {budgets['cancel_p99_ms']:g} ms missed")
        metrics = " | ".join(f"{old[name]:.3f}/{native[name]:.3f}" for name in METRICS)
        lines.append(f"| {key[0]} | {key[1]} | {metrics} | "
                     f"{old['maximum_thread_growth']}/{native['maximum_thread_growth']} | "
                     f"{old['samples']}/{native['samples']} | "
                     f"{old.get('timer_period_ms', 0)}/{native.get('timer_period_ms', 0)} |")
    lines.extend(["", f"Regression budget: max(fallback x {budgets['regression_ratio']:g}, "
                  f"fallback + {budgets['scheduling_tolerance_ms']:g} ms). "
                  "Shared runner scheduling affects these local-workload measurements.", ""])
    lines.extend(f"- {failure}" for failure in failures)
    return "\n".join(lines), failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline")
    parser.add_argument("candidate")
    parser.add_argument("--output", required=True)
    parser.add_argument("--profile", choices=LATENCY_PROFILES, default="strict")
    args = parser.parse_args()
    report, failures = compare(read_evidence(args.baseline), read_evidence(args.candidate), args.profile)
    Path(args.output).write_text(report, encoding="utf-8")
    print(report)
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
