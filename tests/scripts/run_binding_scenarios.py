#!/usr/bin/env python3
"""Run requirement-mapped C++ SDK scenarios against the real TCK SUT."""

import argparse
import os
from pathlib import Path
import socket
import shutil
import subprocess
import tempfile
import time
import uuid


PROFILES = {
    "default": "*-*Negative*:*AuthenticationRules*",
    "authenticated": "*-*Negative*",
    "capability_negative": "*Negative*:*MissingTaskHas*:*CrossBindingTaskHistory*:*DirectMessages*",
}
READY_TIMEOUT = 15
SCENARIO_TIMEOUT = 120
TEST_AUTH_TOKEN = "binding-test-token"


def reserve_ports():
    # Reserve both adjacent ports before releasing them for the SUT.
    for _ in range(100):
        with socket.socket() as http, socket.socket() as grpc:
            http.bind(("127.0.0.1", 0))
            port = http.getsockname()[1]
            if port == 65535:
                continue
            try:
                grpc.bind(("127.0.0.1", port + 1))
                return port
            except OSError:
                continue
    raise RuntimeError("could not reserve adjacent SUT ports")


def wait_until_ready(process, port):
    deadline = time.monotonic() + READY_TIMEOUT
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("SUT exited before becoming ready")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("SUT did not become ready")


def run_profile(args, profile, base_environment):
    environment = dict(base_environment)
    environment["A2A_TCK_PROFILE"] = "capability_negative" if profile == "capability_negative" else "default"
    environment["A2A_TCK_EXTENDED_AGENT_CARD_MODE"] = "configured"
    environment["A2A_TCK_AUTH_TOKEN"] = TEST_AUTH_TOKEN if profile == "authenticated" else ""
    port = reserve_ports()
    environment["A2A_BINDING_SUT_HOST"] = "127.0.0.1"
    environment["A2A_BINDING_SUT_PORT"] = str(port)
    print(f"BIND-EQUIV-001..004: backend={args.backend}, profile={profile}", flush=True)
    with tempfile.TemporaryFile(mode="w+") as log:
        process = subprocess.Popen([str(args.sut), f"127.0.0.1:{port}"], env=environment, stdout=log, stderr=log)
        try:
            wait_until_ready(process, port)
            result = subprocess.run([str(args.scenarios), f"--gtest_filter={PROFILES[profile]}"],
                                    env=environment, timeout=SCENARIO_TIMEOUT, check=False)
            return result.returncode
        finally:
            process.terminate()
            try:
                process.wait(timeout=READY_TIMEOUT)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            log.seek(0)
            print(log.read(), flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sut", type=Path, required=True)
    parser.add_argument("--scenarios", type=Path, required=True)
    parser.add_argument("--backend", choices=("inmemory", "postgres"), required=True)
    args = parser.parse_args()
    environment = dict(os.environ)
    environment["A2A_TCK_STORE_BACKEND"] = args.backend
    # These scenarios do not need extensions; never inherit a TCK client shim.
    environment.pop("A2A_TCK_CLIENT_REQUIRED_EXTENSIONS", None)
    if args.backend == "postgres":
        dsn = environment.get("A2A_TEST_POSTGRES_DSN")
        if not dsn:
            print("SKIP: A2A_TEST_POSTGRES_DSN is required for PostgreSQL scenarios")
            return 77
        environment["A2A_TCK_POSTGRES_DSN"] = dsn
        environment["A2A_TCK_POSTGRES_SCHEMA"] = f"binding_{uuid.uuid4().hex}"
    try:
        for profile in PROFILES:
            result = run_profile(args, profile, environment)
            if result:
                return result
        return 0
    finally:
        if args.backend == "postgres" and shutil.which("psql"):
            # The identifier comes solely from our UUID, never from user input.
            schema = environment["A2A_TCK_POSTGRES_SCHEMA"]
            subprocess.run(["psql", environment["A2A_TCK_POSTGRES_DSN"], "-v", "ON_ERROR_STOP=1",
                            "-c", f'DROP SCHEMA "{schema}" CASCADE'], check=True, capture_output=True)


if __name__ == "__main__":
    raise SystemExit(main())
