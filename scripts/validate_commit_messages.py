#!/usr/bin/env python3
"""Validate Conventional Commit subjects in a Git revision range."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys


ALLOWED_TYPES = (
    "feat",
    "fix",
    "task",
    "docs",
    "perf",
    "refactor",
    "test",
    "build",
    "ci",
    "chore",
    "revert",
)
EXPECTED_FORMAT = "<type>[optional scope][!]: <description>"
SUBJECT_PATTERN = re.compile(
    rf"^(?:{'|'.join(ALLOWED_TYPES)})(?:\([A-Za-z0-9][A-Za-z0-9._/-]*\))?!?: \S(?:.*\S)?$"
)


def is_valid_subject(subject: str) -> bool:
    """Return whether a commit subject follows the repository convention."""
    return SUBJECT_PATTERN.fullmatch(subject) is not None


def git_output(*arguments: str) -> str:
    result = subprocess.run(
        ("git", *arguments),
        check=True,
        stdout=subprocess.PIPE,
        text=True,
    )
    return result.stdout.removesuffix("\n")


def commits_in_pull_request(base: str, head: str) -> list[tuple[str, str]]:
    """Return non-merge commits after the base/head merge base."""
    merge_base = git_output("merge-base", base, head)
    output = git_output(
        "log", "--no-merges", "--format=%H%x00%s", f"{merge_base}..{head}"
    )
    if not output:
        return []
    return [tuple(line.split("\0", 1)) for line in output.splitlines()]


def report_invalid(commit: str, subject: str) -> None:
    allowed = ", ".join(ALLOWED_TYPES)
    print("Invalid commit message:", file=sys.stderr)
    print(f"  SHA: {commit}", file=sys.stderr)
    print(f"  Subject: {subject}", file=sys.stderr)
    print(f"  Expected: {EXPECTED_FORMAT}", file=sys.stderr)
    print(f"  Allowed types: {allowed}", file=sys.stderr)
    print(
        "  Example: fix(http): preserve buffered bytes between requests",
        file=sys.stderr,
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate all non-merge commits introduced by a pull request."
    )
    parser.add_argument("--base", required=True, help="PR base revision")
    parser.add_argument("--head", required=True, help="PR head revision")
    args = parser.parse_args()

    try:
        commits = commits_in_pull_request(args.base, args.head)
    except subprocess.CalledProcessError as error:
        print(f"Unable to determine the pull request commit range: {error}", file=sys.stderr)
        return 2

    invalid = [(commit, subject) for commit, subject in commits if not is_valid_subject(subject)]
    for commit, subject in invalid:
        report_invalid(commit, subject)

    if invalid:
        print(f"Found {len(invalid)} invalid non-merge commit(s).", file=sys.stderr)
        return 1

    print(f"Validated {len(commits)} non-merge commit(s).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
