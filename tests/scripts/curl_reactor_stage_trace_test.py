#!/usr/bin/env python3
"""Ensure the temporary instrumentation cannot leave partial source overlays."""

from pathlib import Path
import shutil
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from trace_curl_reactor import source_overlay

SOURCES = ('src/http/curl_stream_reactor.cpp', 'src/http/curl_windows_readiness.cpp',
           'src/http/http_client_streaming.cpp', 'tests/curl_reactor_evidence.cpp')


class OverlayTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for name in SOURCES:
            destination = self.root / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / name, destination)
        self.original = {name: (self.root / name).read_bytes() for name in SOURCES}

    def test_restores_after_failed_build_or_measurement(self):
        with self.assertRaisesRegex(RuntimeError, 'simulated build failure'):
            with source_overlay(self.root):
                self.assertNotEqual((self.root / SOURCES[0]).read_bytes(), self.original[SOURCES[0]])
                raise RuntimeError('simulated build failure')
        for name, data in self.original.items():
            self.assertEqual((self.root / name).read_bytes(), data)

    def test_invalid_anchor_does_not_mutate_any_source(self):
        path = self.root / SOURCES[-1]
        path.write_text('unsupported source version')
        before = {name: (self.root / name).read_bytes() for name in SOURCES}
        with self.assertRaisesRegex(ValueError, 'anchor changed'):
            with source_overlay(self.root):
                self.fail('invalid overlay applied')
        for name, data in before.items():
            self.assertEqual((self.root / name).read_bytes(), data)

    def test_preserves_concurrent_edit_and_restores_other_sources(self):
        with self.assertRaisesRegex(RuntimeError, 'concurrent source edit'):
            with source_overlay(self.root):
                (self.root / SOURCES[0]).write_text('concurrent edit')
        self.assertEqual((self.root / SOURCES[0]).read_text(), 'concurrent edit')
        for name in SOURCES[1:]:
            self.assertEqual((self.root / name).read_bytes(), self.original[name])

    def test_windows_line_endings_are_restored_byte_for_byte(self):
        for name in SOURCES:
            path = self.root / name
            path.write_bytes(path.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n'))
        before = {name: (self.root / name).read_bytes() for name in SOURCES}
        with source_overlay(self.root):
            self.assertIn(b'stage_trace', (self.root / SOURCES[0]).read_bytes())
        for name, data in before.items():
            self.assertEqual((self.root / name).read_bytes(), data)


if __name__ == '__main__':
    unittest.main()
