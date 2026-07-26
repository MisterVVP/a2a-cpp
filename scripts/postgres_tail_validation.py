#!/usr/bin/env python3
"""Run and summarize the focused PostgreSQL tail-latency validation matrix."""

from __future__ import annotations

import argparse
import csv
import json
import os
import statistics
import subprocess
from pathlib import Path

SCENARIOS = (
    "PushNotify_EndToEndManyConfigs",
    "PushConfig_CreateMany",
    "PushConfig_ListManyConfigs",
    "SendMessage_FollowUpExistingTask",
    "SendMessage_CreateTask",
)
CONCURRENCY_LEVELS = (1, 4, 8)
PHASES = (
    "connection_acquire_wait",
    "task_upsert",
    "push_config_upsert",
    "push_config_list",
    "transaction_begin",
    "transaction_commit",
)
REPETITIONS = 5
REQUESTS = 2_000
WARMUP_SECONDS = 1.0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build/postgres-tail-validation"))
    parser.add_argument("--report-dir", type=Path, default=Path("postgres-tail-validation"))
    return parser.parse_args()


def ensure_driver(build_dir: Path) -> Path:
    driver = build_dir / "tests" / "a2a_performance_driver"
    if driver.exists():
        return driver
    subprocess.run(
        ["cmake", "-S", ".", "-B", str(build_dir), "-DCMAKE_BUILD_TYPE=Release", "-DA2A_ENABLE_TESTING=ON",
         "-DA2A_ENABLE_POSTGRES_STORE=ON"], check=True
    )
    subprocess.run(
        ["cmake", "--build", str(build_dir), "--target", "a2a_performance_driver", "-j", str(os.cpu_count() or 2)],
        check=True,
    )
    return driver


def run_matrix(driver: Path, dsn: str) -> tuple[list[dict[str, object]], str]:
    rows: list[dict[str, object]] = []
    last_schema = ""
    for concurrency in CONCURRENCY_LEVELS:
        for repetition in range(1, REPETITIONS + 1):
            schema = f"a2a_tail_c{concurrency}_r{repetition}_{os.getpid()}"
            env = os.environ.copy()
            env["A2A_TEST_POSTGRES_DSN"] = dsn
            env["A2A_PERF_POSTGRES_SCHEMA"] = schema
            completed = subprocess.run(
                [str(driver), "--transport", "grpc", "--store-backend", "postgres", "--requests", str(REQUESTS),
                 "--concurrency", str(concurrency), "--warmup-seconds", str(WARMUP_SECONDS), "--duration-seconds", "0",
                 "--scenarios", ",".join(SCENARIOS)],
                check=True, capture_output=True, text=True, env=env,
            )
            run_rows = json.loads(completed.stdout)
            for row in run_rows:
                row["repetition"] = repetition
                rows.append(row)
            last_schema = schema
    return rows, last_schema


def median_aggregates(rows: list[dict[str, object]]) -> list[dict[str, object]]:
    aggregates: list[dict[str, object]] = []
    for concurrency in CONCURRENCY_LEVELS:
        for scenario in SCENARIOS:
            selected = [row for row in rows if row["concurrency"] == concurrency and row["scenario"] == scenario]
            aggregate: dict[str, object] = {
                "scenario": scenario,
                "concurrency": concurrency,
                "repetitions": len(selected),
                "throughput_ops_per_sec": statistics.median(float(row["throughput_ops_per_sec"]) for row in selected),
            }
            for metric in ("p95", "p99", "max"):
                aggregate[f"{metric}_ms"] = statistics.median(float(row["latency_ms"][metric]) for row in selected)  # type: ignore[index]
            phase_medians: dict[str, dict[str, float]] = {}
            for phase in PHASES:
                phase_medians[phase] = {
                    metric: statistics.median(
                        float(row["postgres_phase_latency_ms"][phase][metric]) for row in selected  # type: ignore[index]
                    )
                    for metric in ("p95", "p99", "max")
                }
            aggregate["postgres_phase_latency_ms"] = phase_medians
            aggregates.append(aggregate)
    return aggregates


def explain_queries(dsn: str, schema: str) -> str:
    sql = f'''SET search_path TO "{schema}";
SET enable_seqscan = off;
EXPLAIN (ANALYZE, BUFFERS) SELECT task_proto FROM a2a_tasks
 WHERE id = (SELECT id FROM a2a_tasks LIMIT 1);
EXPLAIN (ANALYZE, BUFFERS) SELECT count(*) FROM a2a_push_notification_configs
 WHERE task_id = (SELECT task_id FROM a2a_push_notification_configs LIMIT 1);
EXPLAIN (ANALYZE, BUFFERS) SELECT config_proto FROM a2a_push_notification_configs
 WHERE task_id = (SELECT task_id FROM a2a_push_notification_configs LIMIT 1)
 ORDER BY created_sequence ASC;
'''
    completed = subprocess.run(["psql", dsn, "--no-psqlrc", "--set", "ON_ERROR_STOP=1", "--command", sql],
                               check=True, capture_output=True, text=True)
    output = completed.stdout
    required_indexes = ("a2a_tasks_pkey", "idx_a2a_push_configs_created_sequence")
    missing = [index for index in required_indexes if index not in output]
    if missing:
        raise RuntimeError(f"query-plan review did not use required indexes: {', '.join(missing)}")
    return output


def flat_row(row: dict[str, object]) -> dict[str, object]:
    latency = row["latency_ms"]
    assert isinstance(latency, dict)
    flattened: dict[str, object] = {
        "repetition": row["repetition"], "scenario": row["scenario"], "concurrency": row["concurrency"],
        "operations": row["operations"], "errors": row["errors"],
        "throughput_ops_per_sec": row["throughput_ops_per_sec"], "p95_ms": latency["p95"],
        "p99_ms": latency["p99"], "max_ms": latency["max"],
    }
    phases = row["postgres_phase_latency_ms"]
    assert isinstance(phases, dict)
    for phase in PHASES:
        values = phases[phase]
        assert isinstance(values, dict)
        for metric in ("p95", "p99", "max"):
            flattened[f"{phase}_{metric}_ms"] = values[metric]
    return flattened


def write_reports(report_dir: Path, rows: list[dict[str, object]], aggregates: list[dict[str, object]], plans: str) -> None:
    report_dir.mkdir(parents=True, exist_ok=True)
    payload = {"configuration": {"repetitions": REPETITIONS, "requests": REQUESTS,
                                  "warmup_seconds": WARMUP_SECONDS, "concurrency": CONCURRENCY_LEVELS,
                                  "scenarios": SCENARIOS},
               "results": rows, "median_aggregates": aggregates, "query_plans": plans}
    (report_dir / "results.json").write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    flattened = [flat_row(row) for row in rows]
    with (report_dir / "results.csv").open("w", newline="", encoding="utf-8") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=list(flattened[0]))
        writer.writeheader()
        writer.writerows(flattened)
    lines = ["# PostgreSQL tail-latency validation", "", "## Median aggregates", "",
             "| Scenario | Concurrency | Throughput | p95 ms | p99 ms | Max ms |",
             "| --- | ---: | ---: | ---: | ---: | ---: |"]
    for row in aggregates:
        lines.append(f"| {row['scenario']} | {row['concurrency']} | {float(row['throughput_ops_per_sec']):.2f} | "
                     f"{float(row['p95_ms']):.4f} | {float(row['p99_ms']):.4f} | {float(row['max_ms']):.4f} |")
    lines.extend(["", "## Median PostgreSQL phases", "",
                  "| Scenario | Concurrency | Phase | p95 ms | p99 ms | Max ms |",
                  "| --- | ---: | --- | ---: | ---: | ---: |"])
    for row in aggregates:
        phases = row["postgres_phase_latency_ms"]
        assert isinstance(phases, dict)
        for phase in PHASES:
            values = phases[phase]
            lines.append(f"| {row['scenario']} | {row['concurrency']} | {phase} | {values['p95']:.4f} | "
                         f"{values['p99']:.4f} | {values['max']:.4f} |")
    lines.extend(["", "## Repetitions", "",
                  "| Repetition | Scenario | Concurrency | Throughput | p95 ms | p99 ms | Max ms |",
                  "| ---: | --- | ---: | ---: | ---: | ---: | ---: |"])
    for row in flattened:
        lines.append(f"| {row['repetition']} | {row['scenario']} | {row['concurrency']} | "
                     f"{float(row['throughput_ops_per_sec']):.2f} | {float(row['p95_ms']):.4f} | "
                     f"{float(row['p99_ms']):.4f} | {float(row['max_ms']):.4f} |")
    lines.extend(["", "## Repetition PostgreSQL phases", "",
                  "| Repetition | Scenario | Concurrency | Phase | p95 ms | p99 ms | Max ms |",
                  "| ---: | --- | ---: | --- | ---: | ---: | ---: |"])
    for row in flattened:
        for phase in PHASES:
            lines.append(f"| {row['repetition']} | {row['scenario']} | {row['concurrency']} | {phase} | "
                         f"{float(row[f'{phase}_p95_ms']):.4f} | {float(row[f'{phase}_p99_ms']):.4f} | "
                         f"{float(row[f'{phase}_max_ms']):.4f} |")
    lines.extend(["", "## Query-plan review", "", "```text", plans.rstrip(), "```"])
    (report_dir / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    args = parse_args()
    dsn = os.environ.get("A2A_TEST_POSTGRES_DSN", "")
    if not dsn:
        raise SystemExit("A2A_TEST_POSTGRES_DSN must be set")
    rows, schema = run_matrix(ensure_driver(args.build_dir), dsn)
    errors = sum(int(row["errors"]) for row in rows)
    expected_rows = len(SCENARIOS) * len(CONCURRENCY_LEVELS) * REPETITIONS
    if len(rows) != expected_rows or errors:
        raise RuntimeError(f"validation produced {len(rows)}/{expected_rows} rows and {errors} errors")
    write_reports(args.report_dir, rows, median_aggregates(rows), explain_queries(dsn, schema))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
