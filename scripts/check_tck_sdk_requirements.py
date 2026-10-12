#!/usr/bin/env python3
"""Fail unless the named SDK requirements pass on every supported binding."""

import argparse
import json
from pathlib import Path


REQUIREMENTS = (
    "BIND-EQUIV-001", "BIND-EQUIV-002", "BIND-EQUIV-003", "BIND-EQUIV-004",
    "VER-CLIENT-001", "VER-CLIENT-002", "VER-SERVER-001", "VER-SERVER-002", "VER-SERVER-003",
)
TRANSPORTS = {"http_json", "jsonrpc", "grpc"}


def check_report(report):
    errors = []
    for requirement in REQUIREMENTS:
        result = report.get("per_requirement", {}).get(requirement, {})
        if result.get("status") != "PASS":
            errors.append(f"{requirement}: expected PASS, got {result.get('status', 'MISSING')}")
        bindings = result.get("transports", {})
        for transport in sorted(TRANSPORTS):
            if bindings.get(transport) != "PASS":
                errors.append(f"{requirement}/{transport}: expected PASS, got {bindings.get(transport, 'MISSING')}")
            evidence = f"test_cpp_sdk_requirement[{requirement}-{transport}]"
            if not any(evidence in test_id for test_id in result.get("test_ids", [])):
                errors.append(f"{requirement}/{transport}: missing executable C++ SDK scenario evidence")
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    errors = check_report(json.loads(args.report.read_text(encoding="utf-8")))
    if errors:
        print("\n".join(errors))
        return 1
    print("All nine SDK requirements PASS on HTTP+JSON, JSON-RPC, and gRPC; none skipped or untested.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
