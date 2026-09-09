#!/usr/bin/env python3
"""Unit tests for portable symbol-inspector command construction."""

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from subscription_diagnostics_build_test import symbol_command


class SymbolCommandTest(unittest.TestCase):
    def test_gnu_nm_command(self) -> None:
        self.assertEqual(
            symbol_command("llvm-nm", ["-C"], Path("liba2a_core.a")),
            ["llvm-nm", "-C", "liba2a_core.a"],
        )

    def test_msvc_dumpbin_command(self) -> None:
        self.assertEqual(
            symbol_command("dumpbin.exe", ["/symbols"], Path("a2a_core.lib")),
            ["dumpbin.exe", "/symbols", "a2a_core.lib"],
        )


if __name__ == "__main__":
    unittest.main()
