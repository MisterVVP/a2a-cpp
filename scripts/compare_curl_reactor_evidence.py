#!/usr/bin/env python3
"""Validate equivalent fallback/native reactor workloads and report tail latency."""

import argparse
import json
import math
from pathlib import Path

STREAM_COUNTS = (1, 16, 64, 96)
MODES = ("complete", "cancel")
MAX_THREAD_GROWTH = 8
CANCEL_P95_MS = 10.0
CANCEL_P99_MS = 20.0
REGRESSION_RATIO = 1.25
SCHEDULING_TOLERANCE_MS = 2.0
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
        indexed[key] = row
    expected = {(count, mode) for count in STREAM_COUNTS for mode in MODES}
    if set(indexed) != expected:
        raise ValueError("missing or unexpected workloads")
    return indexed


def compare(baseline, candidate):
    failures = []
    lines = [
        "# Windows libcurl reactor evidence",
        "",
        "Five repetitions per workload; all streams established before cancellation/finish. "
        "Cancellation includes dispatch completion. Zero errors and at most eight added SDK threads required.",
        "",
        "| Streams | Mode | First p95, fallback/native ms | Finish p95, fallback/native ms | "
        "Finish p99, fallback/native ms | Added threads, fallback/native |",
        "| ---: | --- | ---: | ---: | ---: | ---: |",
    ]
    for key, native in candidate.items():
        old = baseline[key]
        if old["samples"] != native["samples"]:
            failures.append(f"{key}: unequal sample counts")
        for metric in METRICS:
            budget = max(old[metric] * REGRESSION_RATIO, old[metric] + SCHEDULING_TOLERANCE_MS)
            if native[metric] > budget:
                failures.append(f"{key}: {metric} {native[metric]:.3f} ms exceeds {budget:.3f} ms")
        if key[1] == "cancel":
            if native["finish_p95_ms"] >= CANCEL_P95_MS or native["finish_p99_ms"] >= CANCEL_P99_MS:
                failures.append(f"{key}: cancellation target p95 < 10 ms / p99 < 20 ms missed")
        metrics = " | ".join(f"{old[name]:.3f}/{native[name]:.3f}" for name in METRICS)
        lines.append(f"| {key[0]} | {key[1]} | {metrics} | "
                     f"{old['maximum_thread_growth']}/{native['maximum_thread_growth']} |")
    lines.extend(["", "Regression budget: 25% or 2 ms above the equivalent fallback, whichever is larger. "
                  "Shared runner scheduling affects these local-workload measurements.", ""])
    lines.extend(f"- {failure}" for failure in failures)
    return "\n".join(lines), failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline")
    parser.add_argument("candidate")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    report, failures = compare(read_evidence(args.baseline), read_evidence(args.candidate))
    Path(args.output).write_text(report, encoding="utf-8")
    print(report)
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
