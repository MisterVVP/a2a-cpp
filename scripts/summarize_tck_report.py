#!/usr/bin/env python3
"""Summarize A2A TCK compatibility gaps from compatibility.json."""

from __future__ import annotations

import argparse
import json
import re
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

FAILED_STATUSES = frozenset({"failed", "fail", "error"})
SKIPPED_STATUSES = frozenset({"skipped", "skip"})
PASSED_STATUSES = frozenset({"passed", "pass", "success"})
NOT_TESTED_STATUSES = frozenset(
    {"not_tested", "not-tested", "not tested", "untested", "not_run", "not-run", "not run", "pending"}
)
STATUS_KEYS = ("status", "result", "outcome")
ID_KEYS = ("requirement_id", "requirementId", "id", "name")
TRANSPORT_KEYS = ("transport", "transports")
ERROR_KEYS = ("error", "message", "failure", "details", "reason")
COUNT_KEYS = {
    "passed": ("passed", "pass", "success"),
    "failed": ("failed", "failures", "failure", "errors", "error"),
    "skipped": ("skipped", "skip"),
    "not-tested": ("not_tested", "notTested", "not-tested", "not tested", "untested", "not_run", "notRun"),
    "total": ("total", "registered", "requirements"),
}
TRANSPORT_NAMES = frozenset({"grpc", "jsonrpc", "http_json", "agent_card"})
REQUIREMENT_ID_PATTERN = re.compile(r"\b[A-Z][A-Z0-9]+(?:-[A-Z0-9]+)+-\d+\b")
PYTEST_SKIP_PATTERN = re.compile(r"^SKIPPED\s+\[[^\]]+\]\s+(?P<case>[^:]+):\s*(?P<reason>.*)$")


@dataclass
class RequirementGap:
    requirement_id: str
    status: str
    transports: set[str] = field(default_factory=set)
    first_error: str = ""
    count: int = 1


@dataclass
class SkippedTestCase:
    name: str
    reason: str
    requirement_ids: set[str] = field(default_factory=set)


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


def integer_value(value: Any) -> int | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, str) and value.strip().isdigit():
        return int(value.strip())
    return None


def count_value(mapping: dict[str, Any], key: str) -> int | None:
    value = first_present(mapping, COUNT_KEYS[key])
    if key == "total" and isinstance(value, list):
        return len(value)
    return integer_value(value)


def label_from_path(path: tuple[str, ...]) -> str:
    return ".".join(path) if path else "aggregate"


def normalize_transport_name(value: Any) -> str | None:
    if not isinstance(value, str):
        return None
    normalized = value.strip()
    if not normalized:
        return None
    return normalized


def collect_transports(node: dict[str, Any], inherited: tuple[str, ...]) -> tuple[str, ...]:
    value = first_present(node, TRANSPORT_KEYS)
    if isinstance(value, str):
        transport = normalize_transport_name(value)
        return (*inherited, transport) if transport is not None else inherited
    if isinstance(value, list):
        return (*inherited, *(transport for item in value if (transport := normalize_transport_name(item))))
    if isinstance(value, dict):
        return (*inherited, *(key for key in value if key.strip().lower() in TRANSPORT_NAMES))
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


def extract_requirement_ids(*values: str) -> set[str]:
    requirement_ids: set[str] = set()
    for value in values:
        requirement_ids.update(REQUIREMENT_ID_PATTERN.findall(value))
    return requirement_ids


def merge_gap(
    gaps: dict[str, RequirementGap],
    requirement_id: str,
    status: str,
    transports: tuple[str, ...],
    error: str,
    count: int = 1,
) -> None:
    existing = gaps.get(requirement_id)
    if existing is None:
        existing = RequirementGap(requirement_id=requirement_id, status=status, count=count)
        gaps[requirement_id] = existing
    if existing.status != "failed" and status == "failed":
        existing.status = status
    existing.transports.update(transports)
    if not existing.first_error and error:
        existing.first_error = error
    existing.count = max(existing.count, count)


def collect_aggregate_counts(
    node: dict[str, Any], gaps: dict[str, RequirementGap], transports: tuple[str, ...], path: tuple[str, ...]
) -> None:
    passed = count_value(node, "passed") or 0
    failed = count_value(node, "failed")
    skipped = count_value(node, "skipped") or 0
    not_tested = count_value(node, "not-tested") or 0
    total = count_value(node, "total")
    if failed is None and total is not None:
        failed = max(total - passed - skipped - not_tested, 0)
    if failed is None:
        failed = 0

    label = label_from_path(path)
    if failed > 0:
        merge_gap(
            gaps,
            f"{label}:aggregate-failed",
            "failed",
            transports,
            "compatibility report contains aggregate failed/untested requirements but no per-requirement IDs here",
            failed,
        )
    if skipped > 0:
        merge_gap(gaps, f"{label}:aggregate-skipped", "skipped", transports, "", skipped)
    if not_tested > 0:
        merge_gap(gaps, f"{label}:aggregate-not-tested", "not-tested", transports, "", not_tested)


def status_from_node(node: dict[str, Any]) -> str | None:
    for key in STATUS_KEYS:
        status = normalize_status(node.get(key))
        if status is not None:
            return status
    return None


def transports_from_status_map(value: Any, status_filter: str | None = None) -> tuple[str, ...]:
    if not isinstance(value, dict):
        return ()
    transports: list[str] = []
    for name, transport_result in value.items():
        if name.strip().lower() not in TRANSPORT_NAMES:
            continue
        if status_filter is None:
            transports.append(name)
            continue
        if isinstance(transport_result, dict) and status_from_node(transport_result) == status_filter:
            transports.append(name)
        elif normalize_status(transport_result) == status_filter:
            transports.append(name)
    return tuple(transports)


def collect_per_requirement(report: Any, gaps: dict[str, RequirementGap]) -> bool:
    if not isinstance(report, dict):
        return False
    per_requirement = report.get("per_requirement")
    if not isinstance(per_requirement, dict):
        return False

    for requirement_id, result in per_requirement.items():
        if not isinstance(requirement_id, str) or not isinstance(result, dict):
            continue
        status = status_from_node(result)
        if status not in {"failed", "skipped", "not-tested"}:
            continue
        transport_map = result.get("transports")
        transports = transports_from_status_map(transport_map, status) or collect_transports(result, ())
        merge_gap(gaps, requirement_id, status, transports, find_error(result))
    return True


def walk(
    node: Any,
    gaps: dict[str, RequirementGap],
    inherited_transports: tuple[str, ...] = (),
    path: tuple[str, ...] = (),
    collect_aggregates: bool = True,
) -> None:
    if isinstance(node, dict):
        transports = collect_transports(node, inherited_transports)
        if collect_aggregates:
            collect_aggregate_counts(node, gaps, transports, path)
        status = status_from_node(node)
        requirement_id = first_present(node, ID_KEYS)
        if status in {"failed", "skipped", "not-tested"} and isinstance(requirement_id, str) and requirement_id.strip():
            merge_gap(gaps, requirement_id.strip(), status, transports, find_error(node))
        for key, value in node.items():
            next_transports = transports
            normalized_key = key.strip().lower() if isinstance(key, str) else str(key)
            if normalized_key in TRANSPORT_NAMES:
                next_transports = (*transports, key)
            walk(value, gaps, next_transports, (*path, key), collect_aggregates)
    elif isinstance(node, list):
        for index, item in enumerate(node):
            walk(item, gaps, inherited_transports, (*path, str(index)), collect_aggregates)


def collect_junit_skips(path: Path) -> list[SkippedTestCase]:
    if not path.exists():
        return []
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError as exc:
        return [SkippedTestCase(name=str(path), reason=f"could not parse JUnit XML: {exc}")]

    skipped_cases: list[SkippedTestCase] = []
    for testcase in root.iter("testcase"):
        skipped = testcase.find("skipped")
        if skipped is None:
            continue
        classname = testcase.attrib.get("classname", "")
        name = testcase.attrib.get("name", "")
        full_name = f"{classname}.{name}" if classname else name
        reason = skipped.attrib.get("message") or (skipped.text or "").strip() or "skipped"
        requirement_ids = extract_requirement_ids(full_name, reason)
        skipped_cases.append(SkippedTestCase(name=full_name, reason=reason, requirement_ids=requirement_ids))
    return skipped_cases


def collect_pytest_skips(path: Path) -> list[SkippedTestCase]:
    if not path.exists():
        return []
    skipped_cases: list[SkippedTestCase] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = PYTEST_SKIP_PATTERN.match(line.strip())
        if match is None:
            continue
        case = match.group("case")
        reason = match.group("reason")
        skipped_cases.append(
            SkippedTestCase(name=case, reason=reason, requirement_ids=extract_requirement_ids(case, reason))
        )
    return skipped_cases


def print_section(title: str, gaps: list[RequirementGap]) -> None:
    print(f"{title} ({len(gaps)})")
    if not gaps:
        print("  none")
        return
    for gap in gaps:
        transports = ", ".join(sorted(gap.transports)) if gap.transports else "unknown"
        count_suffix = f"; count: {gap.count}" if gap.count != 1 else ""
        print(f"  - {gap.requirement_id} [transports: {transports}{count_suffix}]")
        if gap.first_error:
            print(f"    first error: {gap.first_error}")


def print_skipped_tests(skipped_tests: list[SkippedTestCase]) -> None:
    print(f"Skipped test cases ({len(skipped_tests)})")
    if not skipped_tests:
        print("  none")
        return
    for skipped_test in skipped_tests:
        requirement_suffix = ""
        if skipped_test.requirement_ids:
            requirement_suffix = f" [requirements: {', '.join(sorted(skipped_test.requirement_ids))}]"
        print(f"  - {skipped_test.name}{requirement_suffix}")
        print(f"    reason: {skipped_test.reason}")


def default_junit_path(compatibility_json: Path) -> Path:
    return compatibility_json.parent / "junitreport.xml"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compatibility_json", type=Path)
    parser.add_argument(
        "--junit-xml",
        action="append",
        default=[],
        type=Path,
        help="JUnit XML report to scan for skipped pytest cases; defaults to sibling junitreport.xml when present",
    )
    parser.add_argument(
        "--pytest-output",
        action="append",
        default=[],
        type=Path,
        help="pytest console output/log file to scan for skipped case summaries",
    )
    parser.add_argument(
        "--require-zero-gaps",
        action="store_true",
        help="exit non-zero when failed, skipped, or not-tested requirements are present",
    )
    args = parser.parse_args()

    with args.compatibility_json.open("r", encoding="utf-8") as report_file:
        report = json.load(report_file)

    gaps: dict[str, RequirementGap] = {}
    has_per_requirement = collect_per_requirement(report, gaps)
    walk(report, gaps, collect_aggregates=not has_per_requirement)
    failed = sorted((gap for gap in gaps.values() if gap.status == "failed"), key=lambda gap: gap.requirement_id)
    skipped = sorted((gap for gap in gaps.values() if gap.status == "skipped"), key=lambda gap: gap.requirement_id)
    not_tested = sorted((gap for gap in gaps.values() if gap.status == "not-tested"), key=lambda gap: gap.requirement_id)

    junit_paths = list(args.junit_xml)
    sibling_junit = default_junit_path(args.compatibility_json)
    if not junit_paths and sibling_junit.exists():
        junit_paths.append(sibling_junit)
    skipped_tests = []
    for junit_path in junit_paths:
        skipped_tests.extend(collect_junit_skips(junit_path))
    for pytest_output in args.pytest_output:
        skipped_tests.extend(collect_pytest_skips(pytest_output))

    print(f"TCK compatibility gap summary: {args.compatibility_json}")
    print(
        "Note: TCK compatibility percentage excludes skipped tests and NOT TESTED registry requirements; "
        "the sections below list those gaps separately from failed assertions."
    )
    print_section("Failed requirement assertions", failed)
    print_section("Skipped requirement IDs", skipped)
    print_section("Not-tested registry requirement IDs", not_tested)
    print_skipped_tests(skipped_tests)

    gap_count = len(failed) + len(skipped) + len(not_tested)
    if args.require_zero_gaps and gap_count > 0:
        print(f"Found {gap_count} TCK compatibility gap(s).", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
