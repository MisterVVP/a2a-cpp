#!/usr/bin/env python3
"""Check that normal SDK libraries omit subscription diagnostic symbols."""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path

FORBIDDEN_TOKENS = (
    "subscription_diagnostics",
    "A2A_SUBSCRIPTION_DIAGNOSTICS",
    "SSE serialization did not run",
    "JSON-RPC SSE serialization did not run",
)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--symbol-tool", required=True)
    parser.add_argument("--symbol-argument", action="append", default=[])
    parser.add_argument("libraries", nargs="+", type=Path)
    return parser.parse_args()


def symbol_command(tool: str, arguments: list[str], library: Path) -> list[str]:
    return [tool, *arguments, str(library)]


def inspect_library(tool: str, arguments: list[str], library: Path) -> None:
    result = subprocess.run(
        symbol_command(tool, arguments, library),
        check=True,
        capture_output=True,
        text=True,
    )
    symbols = result.stdout + result.stderr
    binary = library.read_bytes()
    for forbidden in FORBIDDEN_TOKENS:
        if forbidden in symbols or forbidden.encode() in binary:
            raise SystemExit(
                f"normal SDK library contains disabled diagnostic token: {forbidden}"
            )


def main() -> int:
    arguments = parse_arguments()
    for library in arguments.libraries:
        inspect_library(
            arguments.symbol_tool, arguments.symbol_argument, library
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
