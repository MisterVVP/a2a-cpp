#!/usr/bin/env python3
"""Summarize A2A TCK compatibility gaps from compatibility.json."""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

FAILED_STATUSES = frozenset({"failed", "fail", "error"})
SKIPPED_STATUSES = frozenset({"skipped", "skip"})
PASSED_STATUSES = frozenset({"passed", "pass", "success"})
NOT_TESTED_STATUSES = frozenset({"not_tested", "not-tested", "not tested", "untested", "not_run", "not-run", "not run", "pending"})
STATUS_KEYS = ("status", "result", "outcome")
ID_KEYS = ("requirement_id", "requirementId", "id", "name")
TRANSPORT_KEYS = ("transport", "transports")
ERROR_KEYS = ("error", "message", "failure", "details", "reason")


@dataclass
class RequirementGap:
    requirement_id: str
    status: str
    transports: set[str] = field(default_factory=set)
    first_error: str = ""


def normalize_status(value: Any) -> str | None:
    if not isinstance(value, str):
        return None
    normalized = value.strip().lower().replace(" ", "_").replace("-", "_")
    if normalized in {status.replace("-", "_").replace(" ", "_") for status in FAILED_STATUSES}:
        return "failed"
    if normalized in SKIPPED_STATUSES:
        return "skipped"
    if normalized in {status.replace("-", "_").replace(" ", "_") for status in NOT_TESTED_STATUSES}:
        return "not-tested"
    if normalized in PASSED_STATUSES:
        return "passed"
    return None


def stringify(value: Any) -> str:
    if isinstance(value, str):
        return value
    if value is None:
        return ""
    if isinstance(value, (int, float, bool)):
        return str(value)
    return json.dumps(value, sort_keys=True)


def first_present(mapping: dict[str, Any], keys: tuple[str, ...]) -> Any:
    for key in keys:
        if key in mapping:
            return mapping[key]
    return None


def collect_transports(node: dict[str, Any], inherited: tuple[str, ...]) -> tuple[str, ...]:
    value = first_present(node, TRANSPORT_KEYS)
    if isinstance(value, str) and value.strip():
        return (*inherited, value.strip())
    if isinstance(value, list):
        return (*inherited, *(str(item) for item in value if str(item).strip()))
    return inherited


def find_error(node: dict[str, Any]) -> str:
    for key in ERROR_KEYS:
        if key in node:
            value = stringify(node[key]).strip()
            if value:
                return value.splitlines()[0]
    for key in ("errors", "failures"):
        value = node.get(key)
        if isinstance(value, list) and value:
            first = value[0]
            if isinstance(first, dict):
                return find_error(first)
            return stringify(first).splitlines()[0]
    return ""


def merge_gap(gaps: dict[str, RequirementGap], requirement_id: str, status: str, transports: tuple[str, ...], error: str) -> None:
    existing = gaps.get(requirement_id)
    if existing is None:
        existing = RequirementGap(requirement_id=requirement_id, status=status)
        gaps[requirement_id] = existing
    if existing.status != "failed" and status == "failed":
        existing.status = status
    existing.transports.update(transports)
    if not existing.first_error and error:
        existing.first_error = error


def walk(node: Any, gaps: dict[str, RequirementGap], inherited_transports: tuple[str, ...] = ()) -> None:
    if isinstance(node, dict):
        transports = collect_transports(node, inherited_transports)
        status = None
        for key in STATUS_KEYS:
            status = normalize_status(node.get(key))
            if status is not None:
                break
        requirement_id = first_present(node, ID_KEYS)
        if status in {"failed", "skipped", "not-tested"} and isinstance(requirement_id, str) and requirement_id.strip():
            merge_gap(gaps, requirement_id.strip(), status, transports, find_error(node))
        for key, value in node.items():
            if key in {"summary", "counts", "totals"}:
                continue
            next_transports = transports
            if key in {"grpc", "jsonrpc", "http_json", "agent_card"}:
                next_transports = (*transports, key)
            walk(value, gaps, next_transports)
    elif isinstance(node, list):
        for item in node:
            walk(item, gaps, inherited_transports)


def print_section(title: str, gaps: list[RequirementGap]) -> None:
    print(f"{title} ({len(gaps)})")
    if not gaps:
        print("  none")
        return
    for gap in gaps:
        transports = ", ".join(sorted(gap.transports)) if gap.transports else "unknown"
        print(f"  - {gap.requirement_id} [transports: {transports}]")
        if gap.first_error:
            print(f"    first error: {gap.first_error}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compatibility_json", type=Path)
    parser.add_argument("--require-zero-gaps", action="store_true", help="exit non-zero when failed, skipped, or not-tested requirements are present")
    args = parser.parse_args()

    with args.compatibility_json.open("r", encoding="utf-8") as report_file:
        report = json.load(report_file)

    gaps: dict[str, RequirementGap] = {}
    walk(report, gaps)
    failed = sorted((gap for gap in gaps.values() if gap.status == "failed"), key=lambda gap: gap.requirement_id)
    skipped = sorted((gap for gap in gaps.values() if gap.status == "skipped"), key=lambda gap: gap.requirement_id)
    not_tested = sorted((gap for gap in gaps.values() if gap.status == "not-tested"), key=lambda gap: gap.requirement_id)

    print(f"TCK compatibility gap summary: {args.compatibility_json}")
    print_section("Failed requirement IDs", failed)
    print_section("Skipped requirement IDs", skipped)
    print_section("Not-tested requirement IDs", not_tested)

    gap_count = len(failed) + len(skipped) + len(not_tested)
    if args.require_zero_gaps and gap_count > 0:
        print(f"Found {gap_count} TCK compatibility gap(s).", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
