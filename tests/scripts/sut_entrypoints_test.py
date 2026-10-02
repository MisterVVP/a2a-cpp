#!/usr/bin/env python3
"""Guard the build and source separation of the two SUT entry points."""

import argparse
import json
from pathlib import Path
import unittest


SUT_DIRECTORY = Path(__file__).resolve().parents[1] / "sut"
DIAGNOSTIC_MARKERS = (
    b"A2A_HTTP_DIAGNOSTICS",
    b"A2A_SUBSCRIPTION_SERVER_DIAGNOSTICS",
    b"/_a2a/performance/reset-subscription-diagnostics",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compile-commands", type=Path)
    parser.add_argument("--tck-sut", type=Path)
    parser.add_argument("--performance-sut", type=Path)
    parser.add_argument("--expect-subscription-diagnostics", action="store_true")
    return parser.parse_args()


class SutEntrypointsTest(unittest.TestCase):
    args: argparse.Namespace

    def test_both_entrypoints_delegate_to_shared_runtime(self) -> None:
        for entrypoint in ("tck_sut.cpp", "performance_sut.cpp"):
            source = (SUT_DIRECTORY / entrypoint).read_text(encoding="utf-8")
            self.assertIn("RunSutRuntime", source)
            self.assertNotIn("GrpcServerTransport", source)
            self.assertNotIn("RestServerTransport", source)
            self.assertNotIn("JsonRpcServerTransport", source)

    def test_conformance_binary_has_no_performance_diagnostics(self) -> None:
        if self.args.tck_sut is None or self.args.performance_sut is None:
            self.skipTest("built SUT paths were not supplied")
        tck_binary = self.args.tck_sut.read_bytes()
        performance_binary = self.args.performance_sut.read_bytes()
        for marker in DIAGNOSTIC_MARKERS:
            self.assertNotIn(marker, tck_binary)
        self.assertIn(DIAGNOSTIC_MARKERS[0], performance_binary)

    def test_diagnostics_definition_is_performance_only(self) -> None:
        if self.args.compile_commands is None:
            self.skipTest("compile_commands.json was not supplied")
        commands = json.loads(self.args.compile_commands.read_text(encoding="utf-8"))
        sut_commands = {
            Path(entry["file"]).name: entry.get("command", " ".join(entry.get("arguments", [])))
            for entry in commands
            if Path(entry["file"]).name in {"sut_runtime.cpp", "tck_sut.cpp", "performance_sut_diagnostics.cpp"}
        }
        diagnostic_definition = "A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS"
        self.assertNotIn(diagnostic_definition, sut_commands["sut_runtime.cpp"])
        self.assertNotIn(diagnostic_definition, sut_commands["tck_sut.cpp"])
        if self.args.expect_subscription_diagnostics:
            self.assertIn(diagnostic_definition, sut_commands["performance_sut_diagnostics.cpp"])
        else:
            self.assertNotIn(diagnostic_definition, sut_commands["performance_sut_diagnostics.cpp"])


if __name__ == "__main__":
    parsed_args = parse_args()
    SutEntrypointsTest.args = parsed_args
    unittest.main(argv=[__file__])
