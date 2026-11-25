# SPDX-License-Identifier: Apache-2.0
"""Summarize interleaved server execution samples as CSV."""

import argparse
import csv
from collections import defaultdict
import json
from pathlib import Path
from statistics import median
import sys


COORDINATES = ("variant", "server_mode", "transport", "scenario", "client_mode", "delay_ms", "concurrency")
TOPOLOGY = ("ingress", "dispatcher_threads", "http_application_threads", "http_network_threads", "http_pending_limit")
RESOURCES = ("server_cpu_seconds", "client_cpu_seconds", "server_peak_rss_kib", "client_peak_rss_kib",
             "server_peak_threads", "client_peak_threads")
METRICS = ("throughput", "p50_ms", "p95_ms", "p99_ms", *RESOURCES)
RANGES = ("throughput", "p99_ms")


def metrics(row):
    return {"throughput": row["throughput_ops_per_sec"],
            **{f"{percentile}_ms": row["latency_ms"][percentile] for percentile in ("p50", "p95", "p99")},
            **{key: row.get(key) for key in RESOURCES}}


def summarize(samples):
    errors = sum(row.get("errors", 0) for row in samples)
    failures = sum("failure" in row for row in samples)
    good = [metrics(row) for row in samples if "failure" not in row and not row.get("errors")]
    counts = {"repetitions": len(samples), "errors": errors, "failed_samples": failures}
    if not good:
        return counts
    for metric in METRICS:
        values = [row[metric] for row in good if row[metric] is not None]
        if not values:
            continue
        counts[f"{metric}_median"] = median(values)
        if metric in RANGES:
            counts[f"{metric}_min"] = min(values)
            counts[f"{metric}_max"] = max(values)
    return counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    args = parser.parse_args()
    groups = defaultdict(list)
    keys = (*COORDINATES, *TOPOLOGY)
    with args.input.open(encoding="utf-8") as source:
        for line in source:
            row = json.loads(line)
            if "configuration" not in row:
                groups[tuple(row.get(key, "") for key in keys)].append(row)
    fields = (*keys, "repetitions", "errors", "failed_samples",
              *(f"{metric}_median" for metric in METRICS),
              *(f"{metric}_{bound}" for metric in RANGES for bound in ("min", "max")))
    writer = csv.DictWriter(sys.stdout, fieldnames=fields)
    writer.writeheader()
    for coordinates, samples in sorted(groups.items(), key=lambda group: tuple(map(str, group[0]))):
        writer.writerow({**dict(zip(keys, coordinates)), **summarize(samples)})


if __name__ == "__main__":
    main()
