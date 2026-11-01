#!/usr/bin/env python3
"""Exercise async SUT discovery, transport selection, and process shutdown."""

import argparse
import json
import os
import signal
import socket
import subprocess
import time
import unittest
import urllib.error
import urllib.request


def listener_port():
    for _ in range(100):
        with socket.socket() as http, socket.socket() as grpc:
            http.bind(("127.0.0.1", 0))
            port = http.getsockname()[1]
            if port == 65535:
                continue
            try:
                grpc.bind(("127.0.0.1", port + 1))
            except OSError:
                continue
            return port
    raise RuntimeError("Could not allocate adjacent SUT listener ports")


class AsyncSutRuntimeTest(unittest.TestCase):
    sut: str

    def test_rejects_unimplemented_async_bindings(self):
        for selection in ("grpc,http_json", "grpc,jsonrpc,http_json", "unknown", "", "grpc,"):
            with self.subTest(transports=selection):
                env = dict(os.environ, A2A_TCK_STORE_BACKEND="inmemory", A2A_TCK_TRANSPORTS=selection)
                result = subprocess.run([self.sut], env=env, capture_output=True, text=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Only grpc is implemented", result.stderr)

    def test_serves_sdk_discovery_without_synchronous_protocol_routes(self):
        port = listener_port()
        env = dict(os.environ, A2A_TCK_STORE_BACKEND="inmemory", A2A_TCK_TRANSPORTS="grpc")
        with subprocess.Popen([self.sut, f"127.0.0.1:{port}"], env=env,
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
                              creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0) as process:
            try:
                self.check_endpoints(process, port)
            finally:
                if os.name == "nt":
                    process.send_signal(signal.CTRL_BREAK_EVENT)
                else:
                    process.terminate()
                try:
                    _, errors = process.communicate(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate()
                    self.fail("Async SUT failed to shut down")
            self.assertEqual(process.returncode, 0, errors)

    def check_endpoints(self, process, port):
        base = f"http://127.0.0.1:{port}"
        deadline = time.monotonic() + 15
        while True:
            try:
                with urllib.request.urlopen(base + "/.well-known/agent-card.json", timeout=1) as response:
                    card = json.load(response)
                    self.assertIn("max-age", response.headers["Cache-Control"])
                break
            except (OSError, urllib.error.URLError):
                if process.poll() is not None or time.monotonic() >= deadline:
                    self.fail("Async SUT did not become ready")
                time.sleep(0.05)
        self.assertEqual(len(card["supportedInterfaces"]), 1)
        interface = card["supportedInterfaces"][0]
        self.assertEqual(interface["protocolBinding"], "GRPC")
        self.assertEqual(interface["url"], f"127.0.0.1:{port + 1}")
        self.assertTrue(card["capabilities"].get("streaming", False))
        for path in ("/rpc", "/a2a/tasks/absent"):
            with self.assertRaises(urllib.error.HTTPError) as raised:
                urllib.request.urlopen(base + path, timeout=1)
            self.assertEqual(raised.exception.code, 404)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--sut", required=True)
    args = parser.parse_args()
    AsyncSutRuntimeTest.sut = args.sut
    unittest.main(argv=[__file__])
