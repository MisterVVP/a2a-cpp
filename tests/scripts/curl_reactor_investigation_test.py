#!/usr/bin/env python3
"""Validate diagnostic joins, timing boundaries, distributions and inputs."""

import copy
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import investigate_curl_reactor as investigation


def fixture():
    batches = []
    evidence = {}
    for count in investigation.STREAM_COUNTS:
        clients = [dict(id=index, submit_ms=1.0, api_return_ms=2.0, accept_begin_ms=-1.0,
                        accepted_ms=3.0, request_ready_ms=4.0, send_begin_ms=5.0, send_end_ms=6.0,
                        metadata_ms=7.0, first_ms=8.0, finished_ms=9.0) for index in range(count)]
        # Socket arrival order differs from submission order. IDs retain the join.
        clients.reverse()
        batches.append(dict(streams=count, repetition=0, mode="complete", warmup=False, clients=clients,
                            reactors_before=dict(created=0), reactors_after_submit=dict(created=4)))
        evidence[(count, "complete")] = dict(samples=count, first_p95_ms=7.0)
    return dict(batches=batches), evidence


class InvestigationTest(unittest.TestCase):
    def test_phase_partition_and_arrival_order(self):
        data, evidence = fixture()
        result = investigation.diagnostic_statistics(data, evidence)
        for stats in result.values():
            self.assertEqual(stats["first"]["p95"], 7.0)
            self.assertEqual(stats["pre-send"]["p95"], 4.0)
            self.assertEqual(stats["post-send"]["p95"], 3.0)
            self.assertEqual(stats["reactors_created"], 4)

    def test_callback_can_arrive_before_submission_returns(self):
        data, evidence = fixture()
        for batch in data["batches"]:
            for client in batch["clients"]:
                client["api_return_ms"] = 8.5
        investigation.diagnostic_statistics(data, evidence)

    def test_incomplete_duplicate_and_impossible_traces_rejected(self):
        data, evidence = fixture()
        mutations = (
            lambda value: value["batches"][0]["clients"].clear(),
            lambda value: value["batches"].append(copy.deepcopy(value["batches"][0])),
            lambda value: value["batches"][0]["clients"][0].update(first_ms=2.0),
            lambda value: value["batches"][0]["clients"][0].update(first_ms=float("nan")),
            lambda value: value["batches"][0]["clients"][0].update(first_ms=8.5),
        )
        for mutate in mutations:
            invalid = copy.deepcopy(data)
            mutate(invalid)
            with self.subTest(mutation=mutate), self.assertRaises(ValueError):
                investigation.diagnostic_statistics(invalid, evidence)

    def test_warmup_is_retained_but_excluded_from_distributions(self):
        data, evidence = fixture()
        warmup = copy.deepcopy(data["batches"][0])
        warmup["warmup"] = True
        warmup["clients"][0]["first_ms"] = 1000.0
        data["batches"].insert(0, warmup)
        result = investigation.diagnostic_statistics(data, evidence)
        self.assertEqual(result[1]["first"]["max"], 7.0)

    def test_fingerprint_includes_runtime_dlls(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            executable = path / "evidence.exe"
            executable.write_bytes(b"same executable")
            library = path / "a2a_http.dll"
            library.write_bytes(b"native backend")
            native = investigation.binary_fingerprint(executable)
            library.write_bytes(b"fallback backend")
            fallback = investigation.binary_fingerprint(executable)
            self.assertNotEqual(native, fallback)

    def test_percentile_matches_generator_nearest_rank(self):
        self.assertEqual(investigation.percentile(list(range(1, 21)), 0.95), 19)

    def test_failed_baseline_build_restores_native_configuration(self):
        portable = False
        commands = []

        def run(command, **_kwargs):
            nonlocal portable
            commands.append(command)
            if command[-1].startswith("-DA2A_TEST_CURL_PORTABLE_REACTOR="):
                portable = command[-1].endswith("=ON")
            elif "--build" in command and portable:
                raise investigation.subprocess.CalledProcessError(1, command)

        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            (build / "CMakeCache.txt").write_text("configured")
            executable = build / "tests" / "RelWithDebInfo" / "curl_reactor_evidence.exe"
            executable.parent.mkdir(parents=True)
            executable.write_bytes(b"native")
            with mock.patch.object(investigation.subprocess, "run", side_effect=run):
                with self.assertRaises(investigation.subprocess.CalledProcessError):
                    investigation.prepare_binaries(build, "RelWithDebInfo")
            self.assertFalse(portable)
            self.assertIn("--build", commands[-1])


if __name__ == "__main__":
    unittest.main()
