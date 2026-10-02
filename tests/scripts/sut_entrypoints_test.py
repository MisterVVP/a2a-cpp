#!/usr/bin/env python3
"""Guard the separation between conformance and performance SUT entry points."""

from pathlib import Path
import unittest


SUT_DIRECTORY = Path(__file__).resolve().parents[1] / "sut"


class SutEntrypointsTest(unittest.TestCase):
    def test_both_entrypoints_delegate_to_shared_runtime(self) -> None:
        for entrypoint in ("tck_sut.cpp", "performance_sut.cpp"):
            source = (SUT_DIRECTORY / entrypoint).read_text(encoding="utf-8")
            self.assertIn("RunSutRuntime", source)
            self.assertNotIn("GrpcServerTransport", source)
            self.assertNotIn("RestServerTransport", source)
            self.assertNotIn("JsonRpcServerTransport", source)

    def test_conformance_entrypoint_has_no_performance_diagnostics(self) -> None:
        source = (SUT_DIRECTORY / "tck_sut.cpp").read_text(encoding="utf-8")
        self.assertNotIn("A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS", source)
        self.assertNotIn("A2A_HTTP_DIAGNOSTICS", source)
        self.assertNotIn("A2A_SUBSCRIPTION_SERVER_DIAGNOSTICS", source)
        self.assertNotIn("enable_http_diagnostics = true", source)


if __name__ == "__main__":
    unittest.main()
