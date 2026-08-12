#!/usr/bin/env python3

import importlib.util
import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
VALIDATOR_PATH = REPOSITORY_ROOT / "scripts" / "validate_commit_messages.py"
SPEC = importlib.util.spec_from_file_location("validate_commit_messages", VALIDATOR_PATH)
assert SPEC is not None and SPEC.loader is not None
VALIDATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VALIDATOR)


class SubjectValidationTest(unittest.TestCase):
    def test_accepts_supported_subjects(self) -> None:
        valid_subjects = (
            "fix: correct parsing",
            "fix: x",
            "feat(http): reuse connections",
            "task: add diagnostics",
            "docs: explain commits",
            "perf(streaming): reduce latency",
            "refactor(server): extract state",
            "test: cover requests",
            "build: update configuration",
            "ci: validate commits",
            "chore: update metadata",
            "revert: restore prior behavior",
            "feat(api)!: change transport interface",
        )
        for subject in valid_subjects:
            with self.subTest(subject=subject):
                self.assertTrue(VALIDATOR.is_valid_subject(subject))

    def test_rejects_free_form_and_malformed_subjects(self) -> None:
        invalid_subjects = (
            "address review",
            "fix test complexity",
            "updates",
            "unknown: add behavior",
            "fix: ",
            "fix: trailing space ",
            "fix(): empty scope",
        )
        for subject in invalid_subjects:
            with self.subTest(subject=subject):
                self.assertFalse(VALIDATOR.is_valid_subject(subject))


class CommitRangeValidationTest(unittest.TestCase):
    def run_git(self, repository: Path, *arguments: str) -> str:
        result = subprocess.run(
            ("git", *arguments),
            cwd=repository,
            check=True,
            stdout=subprocess.PIPE,
            text=True,
        )
        return result.stdout.strip()

    def commit(self, repository: Path, subject: str, content: str) -> str:
        (repository / "content.txt").write_text(content, encoding="utf-8")
        self.run_git(repository, "add", "content.txt")
        self.run_git(repository, "commit", "-m", subject)
        return self.run_git(repository, "rev-parse", "HEAD")

    def test_validates_every_non_merge_commit_after_merge_base(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            self.run_git(repository, "init", "-b", "main")
            self.run_git(repository, "config", "user.name", "Test Author")
            self.run_git(repository, "config", "user.email", "author@example.com")
            base = self.commit(repository, "chore: initialize fixture", "base")
            self.run_git(repository, "switch", "-c", "feature")
            invalid = self.commit(repository, "address review", "invalid")
            valid = self.commit(repository, "fix(http): preserve bytes", "valid")

            result = subprocess.run(
                (str(VALIDATOR_PATH), "--base", base, "--head", valid),
                cwd=repository,
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

            self.assertEqual(result.returncode, 1)
            self.assertIn(invalid, result.stderr)
            self.assertIn("address review", result.stderr)
            self.assertIn(VALIDATOR.EXPECTED_FORMAT, result.stderr)
            self.assertIn("Allowed types:", result.stderr)


if __name__ == "__main__":
    unittest.main()
