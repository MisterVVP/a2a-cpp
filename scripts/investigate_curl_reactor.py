#!/usr/bin/env python3
"""Collect balanced-order reactor evidence and opt-in first-chunk diagnostics.

Exit success means collection/validation succeeded, not that strict latency
acceptance passed. Every strict comparison and its failures are retained.
"""

import argparse
import hashlib
import json
import math
import os
import platform
import shutil
import subprocess
import sys
import time
from pathlib import Path

from compare_curl_reactor_evidence import STREAM_COUNTS, compare, read_evidence

DIAGNOSTIC_REPETITIONS = 5
SCENARIOS = {
    "fresh": [],
    "traced-fresh": ["--complete-only", "--repetitions", str(DIAGNOSTIC_REPETITIONS)],
    "traced-reused": ["--complete-only", "--repetitions", str(DIAGNOSTIC_REPETITIONS), "--reuse-client", "--warmup"],
}
PHASES = {
    "first": ("first_ms", "submit_ms"),
    "api": ("api_return_ms", "submit_ms"),
    "pre-send": ("send_begin_ms", "submit_ms"),
    "server-read": ("request_ready_ms", "accepted_ms"),
    "post-send": ("first_ms", "send_begin_ms"),
    "metadata-to-first": ("first_ms", "metadata_ms"),
}
SUMMARY_TOLERANCE_MS = 0.001


def percentile(values, quantile):
    ordered = sorted(values)
    if not ordered:
        raise ValueError("empty latency distribution")
    return ordered[math.ceil(len(ordered) * quantile) - 1]


def diagnostic_statistics(data, evidence):
    """Join by the request's explicit ID, never by socket acceptance order."""
    grouped = {count: [] for count in STREAM_COUNTS}
    lifecycle = {count: [] for count in STREAM_COUNTS}
    cpu = {count: [] for count in STREAM_COUNTS}
    repetitions = {count: set() for count in STREAM_COUNTS}
    for batch in data["batches"]:
        if batch["warmup"]:
            continue
        count = batch["streams"]
        if count not in grouped or batch["mode"] != "complete":
            raise ValueError("unexpected diagnostic workload")
        repeat = batch["repetition"]
        if repeat in repetitions[count]:
            raise ValueError("duplicate diagnostic repetition")
        repetitions[count].add(repeat)
        clients = batch["clients"]
        if len(clients) != count or {row["id"] for row in clients} != set(range(count)):
            raise ValueError("incomplete diagnostic request IDs")
        for row in clients:
            if not all(math.isfinite(value) for key, value in row.items() if key != "id"):
                raise ValueError("non-finite diagnostic timestamp")
            if not (row["submit_ms"] <= row["accepted_ms"] <= row["request_ready_ms"] <= row["send_begin_ms"]
                    <= row["first_ms"] <= row["finished_ms"]):
                raise ValueError("invalid diagnostic event order")
            if not row["submit_ms"] <= row["metadata_ms"] <= row["first_ms"]:
                raise ValueError("invalid metadata dispatch order")
            grouped[count].append(row)
        lifecycle[count].append(batch["reactors_after_submit"]["created"] - batch["reactors_before"]["created"])
        cpu[count].append(batch.get("process_cpu_ms", 0.0))
    result = {}
    for count, rows in grouped.items():
        summary = evidence[(count, "complete")]
        if len(rows) != summary["samples"]:
            raise ValueError("diagnostics and summary sample counts differ")
        stats = {}
        for name, (end, start) in PHASES.items():
            values = [row[end] - row[start] for row in rows]
            stats[name] = {f"p{int(q * 100)}": percentile(values, q) for q in (0.5, 0.9, 0.95, 0.99)}
            stats[name]["max"] = max(values)
        if abs(stats["first"]["p95"] - summary["first_p95_ms"]) > SUMMARY_TOLERANCE_MS:
            raise ValueError("raw first-event p95 differs from summary")
        cutoff = stats["first"]["p95"]
        tail = [row for row in rows if row["first_ms"] - row["submit_ms"] >= cutoff]
        stats["tail_pre_send_mean_ms"] = sum(row["send_begin_ms"] - row["submit_ms"] for row in tail) / len(tail)
        stats["tail_post_send_mean_ms"] = sum(row["first_ms"] - row["send_begin_ms"] for row in tail) / len(tail)
        stats["repetitions"] = len(repetitions[count])
        stats["repetitions_creating_reactors"] = sum(value > 0 for value in lifecycle[count])
        stats["reactors_created"] = sum(lifecycle[count])
        stats["process_cpu_p50_ms"] = percentile(cpu[count], 0.5)
        stats["process_cpu_p95_ms"] = percentile(cpu[count], 0.95)
        result[count] = stats
    return result


def cpu_name():
    if sys.platform == "win32":
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:
                return winreg.QueryValueEx(key, "ProcessorNameString")[0].strip()
        except OSError:
            pass
    return platform.processor()


def loopback_time_wait():
    if sys.platform != "win32":
        return None
    result = subprocess.run(["netstat", "-an", "-p", "tcp"], capture_output=True, text=True, check=True)
    return sum("127.0.0.1" in line and "TIME_WAIT" in line for line in result.stdout.splitlines())


def execute(binary, name, scenario, directory):
    command = [str(binary), *SCENARIOS[scenario]]
    if scenario.startswith("traced-"):
        command.extend(["--diagnostics", str(directory / f"{name}-timing.json")])
    before = loopback_time_wait()
    started = time.monotonic()
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    path = directory / f"{name}.json"
    path.write_text(result.stdout, encoding="utf-8")
    (directory / f"{name}.stderr").write_text(result.stderr, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"{name}/{scenario} exited {result.returncode}: {result.stderr}")
    modes = ("complete",) if scenario.startswith("traced-") else ("complete", "cancel")
    evidence = read_evidence(path, modes)
    record = dict(command=command, elapsed_seconds=time.monotonic() - started,
                  time_wait_before=before, time_wait_after=loopback_time_wait())
    if scenario.startswith("traced-"):
        diagnostic = json.loads((directory / f"{name}-timing.json").read_text(encoding="utf-8-sig"))
        record["diagnostic_environment"] = {key: value for key, value in diagnostic.items() if key != "batches"}
        record["phases"] = diagnostic_statistics(diagnostic, evidence)
    return evidence, record


def phase_report(name, statistics):
    lines = [f"### {name}", "",
             "P50 / P95 / P99 / max, ms. Pre-send + post-send sum to first latency per sample; their percentiles do not.",
             "", "| Streams | First | API call | Pre-send | Server read | Post-send | Process CPU p50/p95 | Reactors created / repetitions |",
             "| ---: | --- | --- | --- | --- | --- | --- | --- |"]
    for count, stats in statistics.items():
        cells = [" / ".join(f"{stats[phase][q]:.3f}" for q in ("p50", "p95", "p99", "max"))
                 for phase in ("first", "api", "pre-send", "server-read", "post-send")]
        lines.append(f"| {count} | " + " | ".join(cells) +
                     f" | {stats['process_cpu_p50_ms']:.3f} / {stats['process_cpu_p95_ms']:.3f}" +
                     f" | {stats['reactors_created']} / {stats['repetitions_creating_reactors']} |")
    return lines


def binary_fingerprint(path):
    files = [path, *sorted(path.parent.glob("*.dll"))]
    return {file.name: hashlib.sha256(file.read_bytes()).hexdigest() for file in files}


def prepare_binaries(build, configuration):
    if not (build / "CMakeCache.txt").is_file():
        raise ValueError("configure the Windows build with libcurl/testing before this command")
    repository = Path(__file__).resolve().parents[1]
    environment = os.environ.copy()
    if "CI_VCPKG_ROOT" in environment:
        environment["VCPKG_ROOT"] = environment["CI_VCPKG_ROOT"]
    commands = []

    def configure_and_build(portable):
        configure = ["cmake", "-S", str(repository), "-B", str(build),
                     f"-DA2A_TEST_CURL_PORTABLE_REACTOR={'ON' if portable else 'OFF'}"]
        compile_command = ["cmake", "--build", str(build), "--config", configuration,
                           "--target", "curl_reactor_evidence", "--parallel"]
        for command in (configure, compile_command):
            commands.append(command)
            subprocess.run(command, env=environment, check=True)

    binaries = {}
    try:
        for name, portable in (("native", False), ("fallback", True)):
            configure_and_build(portable)
            candidates = (build / "tests" / configuration / "curl_reactor_evidence.exe",
                          build / "tests" / "curl_reactor_evidence.exe")
            source = next((path for path in candidates if path.is_file()), None)
            if source is None:
                raise ValueError("Windows evidence executable was not produced")
            destination = build / "reactor-investigation-binaries" / name
            destination.mkdir(parents=True, exist_ok=True)
            for path in (source, *source.parent.glob("*.dll"), *source.parent.glob("curl_reactor_evidence.pdb")):
                shutil.copy2(path, destination / path.name)
            binaries[name] = destination / source.name
    finally:
        # Restore the configured native backend even if the baseline build fails.
        configure_and_build(False)
    return binaries, commands


def cache_configuration(path):
    if path is None:
        return {}
    selected = {"CMAKE_GENERATOR", "CMAKE_CXX_COMPILER", "CMAKE_CXX_FLAGS", "CMAKE_CXX_FLAGS_RELWITHDEBINFO",
                "CMAKE_CXX_FLAGS_RELEASE", "CMAKE_BUILD_TYPE", "BUILD_SHARED_LIBS", "VCPKG_TARGET_TRIPLET",
                "VCPKG_HOST_TRIPLET", "A2A_ENABLE_ASIO", "A2A_ENABLE_SIGNING"}
    result = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        key = line.split(":", 1)[0]
        if key in selected and "=" in line:
            result[key] = line.split("=", 1)[1]
    return result


def collect(native, fallback, output, configuration, cmake_cache=None):
    output.mkdir(parents=True, exist_ok=True)
    binaries = {"native": native.resolve(), "fallback": fallback.resolve()}
    hashes = {name: binary_fingerprint(path) for name, path in binaries.items()}
    if hashes["native"] == hashes["fallback"]:
        raise ValueError("native and fallback executable snapshots are identical")
    manifest = dict(platform=platform.platform(), cpu=cpu_name(), logical_cpus=os.cpu_count(),
                    configuration=configuration, common_cmake=cache_configuration(cmake_cache),
                    binary_sha256=hashes, runs=[])
    report = ["# Windows first-event investigation", "",
              "Strict limits are unchanged. Successful collection does not imply strict acceptance.",
              "Traced runs add request IDs and test-only timestamps; use untraced fresh runs for acceptance.", ""]
    for order_name, order in (("native-first", ("native", "fallback")), ("fallback-first", ("fallback", "native"))):
        for scenario in SCENARIOS:
            directory = output / order_name / scenario
            directory.mkdir(parents=True, exist_ok=True)
            evidence = {}
            records = {}
            for name in order:
                print(f"Collecting {order_name}/{scenario}/{name}", flush=True)
                evidence[name], records[name] = execute(binaries[name], name, scenario, directory)
            if scenario.startswith("traced-"):
                if (records["native"]["diagnostic_environment"]["backend"] != "wsapoll" or
                        records["fallback"]["diagnostic_environment"]["backend"] != "portable"):
                    raise ValueError("incorrect native/fallback executable labels")
                for key in ("compiler", "libcurl", "timer_period_ms", "hardware_threads", "client_lifetime"):
                    if records["native"]["diagnostic_environment"][key] != records["fallback"]["diagnostic_environment"][key]:
                        raise ValueError(f"unmatched diagnostic environment: {key}")
            comparison, failures = compare(evidence["fallback"], evidence["native"])
            (directory / "strict.md").write_text(comparison, encoding="utf-8")
            manifest["runs"].append(dict(order=order_name, scenario=scenario, executions=records, strict_failures=failures))
            report.extend([f"## {order_name}: {scenario}", "", comparison, ""])
            for name in order:
                if "phases" in records[name]:
                    report.extend(phase_report(name, records[name]["phases"]))
                    report.append("")
            (output / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
            (output / "summary.md").write_text("\n".join(report), encoding="utf-8")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--build-dir", type=Path)
    inputs.add_argument("--native", type=Path)
    parser.add_argument("--fallback", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--configuration", required=True)
    parser.add_argument("--cmake-cache", type=Path)
    args = parser.parse_args()
    build_commands = []
    if args.build_dir:
        binaries, build_commands = prepare_binaries(args.build_dir.resolve(), args.configuration)
        native, fallback = binaries["native"], binaries["fallback"]
        cache = args.build_dir / "CMakeCache.txt"
    elif args.fallback:
        native, fallback, cache = args.native, args.fallback, args.cmake_cache
    else:
        parser.error("--fallback is required with --native")
    args.output_dir = args.output_dir.resolve()
    manifest = collect(native, fallback, args.output_dir, args.configuration, cache)
    manifest["build_commands"] = build_commands
    (args.output_dir / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
