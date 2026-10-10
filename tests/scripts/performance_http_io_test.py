#!/usr/bin/env python3
"""Cross-platform wire regressions for the bounded asynchronous SUT."""

from __future__ import annotations

import contextlib
import json
import os
import socket
import subprocess
import sys
import tempfile
import unittest

from http_persistence_test import (
    HOST, HTTP_OK, HEADER_END, SHUTDOWN_TIMEOUT_SECONDS, available_port,
    assert_connection_closed, connect, read_response, request,
    request_graceful_shutdown, wait_until_ready,
)

SUT = sys.argv.pop(1)
REST_SEND = "/a2a/message:send"
REST_STREAM = "/a2a/message:stream"
RPC_PATH = "/rpc"
MESSAGE_ID = "async-io-test"
TASK_ID = "async-io-task"
TEXT = "wire event"
LARGE_TEXT_BYTES = 256 * 1024
IDLE_CONNECTIONS = 24
SLOW_CONNECTIONS = 6
APPLICATION_WORKERS = 2
MAX_CONNECTIONS = 32
SMALL_INPUT_BYTES = 512
WRITE_SLICE_BYTES = 17
HEARTBEAT_TIMEOUT_SECONDS = 20
SSE_COMMENT_PREFIX = b":"
PIPELINE_REQUESTS = 8
LINE_END = b"\r\n"
RESOURCE_PREFIX = "A2A_HTTP_IO_RESOURCES"
REQUEST_HEADERS = {"A2A-Version": "1.0", "A2A-Extensions": "urn:a2a:tck:required-extension", "Content-Type": "application/json"}


def send_body(text: str = TEXT, message_id: str = MESSAGE_ID) -> dict:
    return {"message": {"messageId": message_id, "role": "ROLE_USER", "parts": [{"text": text}]}}


def wire_request(path: str, body: dict, close: bool = False) -> bytes:
    payload = json.dumps(body).encode()
    lines = [f"POST {path} HTTP/1.1", "Host: localhost"]
    lines.extend(f"{key}: {value}" for key, value in REQUEST_HEADERS.items())
    lines.extend([f"Content-Length: {len(payload)}", f"Connection: {'close' if close else 'keep-alive'}"])
    return LINE_END.join(line.encode() for line in lines) + HEADER_END + payload


def rpc_request(method: str, params: dict) -> bytes:
    return wire_request(RPC_PATH, {"jsonrpc": "2.0", "id": MESSAGE_ID, "method": method, "params": params})


class Reader:
    def __init__(self, client: socket.socket):
        self.client = client
        self.buffer = b""

    def until(self, delimiter: bytes) -> bytes:
        while delimiter not in self.buffer:
            received = self.client.recv(4096)
            if not received:
                raise AssertionError("unexpected response EOF")
            self.buffer += received
        result, self.buffer = self.buffer.split(delimiter, 1)
        return result

    def exact(self, size: int) -> bytes:
        while len(self.buffer) < size:
            received = self.client.recv(4096)
            if not received:
                raise AssertionError("unexpected body EOF")
            self.buffer += received
        result, self.buffer = self.buffer[:size], self.buffer[size:]
        return result

    def headers(self) -> bytes:
        headers = self.until(HEADER_END)
        assert headers.startswith(HTTP_OK), headers
        return headers

    def chunk(self) -> bytes:
        size = int(self.until(LINE_END), 16)
        payload = self.exact(size)
        assert self.exact(len(LINE_END)) == LINE_END
        return payload


@contextlib.contextmanager
def running_sut(**limits: int):
    port = available_port()
    env = os.environ.copy()
    for name in tuple(env):
        if name.startswith(("A2A_TCK_", "A2A_PERF_HTTP_")):
            del env[name]
    env["A2A_TCK_STORE_BACKEND"] = "inmemory"
    defaults = {"WORKERS": APPLICATION_WORKERS, "MAX_CONNECTIONS": MAX_CONNECTIONS}
    defaults.update(limits)
    env.update({f"A2A_PERF_HTTP_{name}": str(value) for name, value in defaults.items()})
    flags = subprocess.CREATE_NEW_PROCESS_GROUP if sys.platform == "win32" else 0
    with tempfile.TemporaryFile() as log:
        process = subprocess.Popen([SUT, f"{HOST}:{port}"], stdout=log, stderr=log, env=env, creationflags=flags)
        try:
            wait_until_ready(process, port)
            yield port, process
            request_graceful_shutdown(process)
            process.wait(timeout=SHUTDOWN_TIMEOUT_SECONDS)
            log.seek(0)
            output = log.read().decode(errors="replace")
            assert process.returncode == 0, output
            resource_line = next(line for line in output.splitlines() if line.startswith(RESOURCE_PREFIX))
            values = dict(item.split("=", 1) for item in resource_line.split()[1:])
            assert int(values["io_workers"]) == 1
            assert int(values["application_workers"]) == APPLICATION_WORKERS
            assert int(values["peak_connections"]) <= defaults["MAX_CONNECTIONS"]
            assert int(values["peak_queued_input_bytes"]) <= int(values["max_input_bytes"]) * defaults["MAX_CONNECTIONS"]
            assert int(values["peak_queued_output_bytes"]) <= int(values["max_output_bytes"]) * defaults["MAX_CONNECTIONS"]
        except Exception:
            log.seek(0)
            sys.stderr.write(log.read().decode(errors="replace"))
            raise
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=SHUTDOWN_TIMEOUT_SECONDS)


class PerformanceHttpIoTest(unittest.TestCase):
    def test_finite_stream_pipeline_and_partial_writes(self):
        with running_sut(WRITE_SLICE_BYTES=WRITE_SLICE_BYTES) as (port, _):
            for payload in (wire_request(REST_STREAM, send_body()), rpc_request("SendStreamingMessage", send_body())):
                with connect(port) as client:
                    client.sendall(payload + request(b"close"))
                    reader = Reader(client)
                    self.assertIn(b"Transfer-Encoding: chunked", reader.headers())
                    chunks = []
                    while chunk := reader.chunk():
                        chunks.append(chunk)
                    self.assertTrue(chunks)
                    response, carry = read_response(client, reader.buffer)
                    self.assertTrue(response.startswith(HTTP_OK), response)
                    self.assertFalse(carry)
                    assert_connection_closed(client)

    def test_subscription_cancellation_and_disconnect(self):
        with running_sut() as (port, _):
            for rpc in (False, True):
                with connect(port) as control, connect(port) as stream:
                    seed = rpc_request("SendMessage", send_body()) if rpc else wire_request(REST_SEND, send_body())
                    control.sendall(seed)
                    response, _ = read_response(control)
                    task = json.loads(response.split(HEADER_END, 1)[1])
                    task_id = task["result"]["task"]["id"] if rpc else task["task"]["id"]
                    subscribe = rpc_request("SubscribeToTask", {"id": task_id}) if rpc else wire_request(f"/a2a/tasks/{task_id}:subscribe", {})
                    stream.sendall(subscribe)
                    reader = Reader(stream)
                    reader.headers()
                    self.assertIn(b"data:", reader.chunk())
                    # The subscriber is idle; a control request must remain serviceable.
                    cancel = rpc_request("CancelTask", {"id": task_id}) if rpc else wire_request(f"/a2a/tasks/{task_id}:cancel", {})
                    control.sendall(cancel)
                    canceled, _ = read_response(control)
                    self.assertTrue(canceled.startswith(HTTP_OK), canceled)
                    self.assertIn(b"CANCELED", reader.chunk().upper())
                    self.assertEqual(reader.chunk(), b"")
                    stream.sendall(request(b"close"))
                    reused, _ = read_response(stream, reader.buffer)
                    self.assertTrue(reused.startswith(HTTP_OK))
                # A reset/disconnect while a subscription is idle is covered below
                # by shutting down with its initial event already delivered.

    def test_live_heartbeat_then_cancellation_and_reuse(self):
        with running_sut() as (port, _):
            with connect(port) as control, connect(port) as stream:
                control.sendall(wire_request(REST_SEND, send_body()))
                response, _ = read_response(control)
                task_id = json.loads(response.split(HEADER_END, 1)[1])["task"]["id"]
                stream.sendall(wire_request(f"/a2a/tasks/{task_id}:subscribe", {}))
                reader = Reader(stream)
                reader.headers()
                self.assertIn(b"data:", reader.chunk())
                stream.settimeout(HEARTBEAT_TIMEOUT_SECONDS)
                self.assertTrue(reader.chunk().startswith(SSE_COMMENT_PREFIX))
                control.sendall(wire_request(f"/a2a/tasks/{task_id}:cancel", {}))
                canceled, _ = read_response(control)
                self.assertTrue(canceled.startswith(HTTP_OK))
                self.assertIn(b"CANCELED", reader.chunk().upper())
                self.assertEqual(reader.chunk(), b"")
                stream.sendall(request(b"close"))
                reused, _ = read_response(stream, reader.buffer)
                self.assertTrue(reused.startswith(HTTP_OK))
                assert_connection_closed(stream)

    def test_idle_and_slow_readers_do_not_hold_workers(self):
        with contextlib.ExitStack() as clients, running_sut(WRITE_SLICE_BYTES=WRITE_SLICE_BYTES) as (port, _):
            idle = [clients.enter_context(connect(port)) for _ in range(IDLE_CONNECTIONS)]
            for client in idle:
                client.sendall(request())
                response, _ = read_response(client)
                self.assertTrue(response.startswith(HTTP_OK))
            for _ in range(SLOW_CONNECTIONS):
                client = clients.enter_context(connect(port))
                client.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
                client.sendall(wire_request(REST_SEND, send_body("x" * LARGE_TEXT_BYTES)))
            with connect(port) as fast:
                fast.sendall(request(b"close"))
                response, _ = read_response(fast)
                self.assertTrue(response.startswith(HTTP_OK))
            # Shutdown happens while sockets and queued output remain alive.

    def test_shutdown_cancels_idle_subscription_and_fragmented_read(self):
        with contextlib.ExitStack() as clients, running_sut() as (port, _):
            control = clients.enter_context(connect(port))
            control.sendall(wire_request(REST_SEND, send_body()))
            response, _ = read_response(control)
            task_id = json.loads(response.split(HEADER_END, 1)[1])["task"]["id"]
            stream = clients.enter_context(connect(port))
            stream.sendall(wire_request(f"/a2a/tasks/{task_id}:subscribe", {}))
            reader = Reader(stream)
            reader.headers()
            self.assertIn(b"data:", reader.chunk())
            fragmented = clients.enter_context(connect(port))
            fragmented.sendall(request()[:WRITE_SLICE_BYTES])
            # Fixture shutdown runs before these sockets are closed.

    def test_output_budget_rejects_large_response_and_preserves_other_clients(self):
        with running_sut(MAX_OUTPUT_BYTES=SMALL_INPUT_BYTES) as (port, _):
            with connect(port) as slow:
                slow.sendall(wire_request(REST_SEND, send_body("x" * SMALL_INPUT_BYTES)))
                assert_connection_closed(slow)
            with connect(port) as fast:
                fast.sendall(request(b"close"))
                response, _ = read_response(fast)
                self.assertTrue(response.startswith(HTTP_OK))

    def test_full_input_buffer_pauses_and_resumes_pipeline(self):
        with running_sut(MAX_INPUT_BYTES=SMALL_INPUT_BYTES) as (port, _):
            with connect(port) as client:
                client.sendall(request() * PIPELINE_REQUESTS + request(b"close"))
                carry = b""
                for _ in range(PIPELINE_REQUESTS + 1):
                    response, carry = read_response(client, carry)
                    self.assertTrue(response.startswith(HTTP_OK))
                assert_connection_closed(client)

    def test_input_budget_eof_and_accept_resumption(self):
        with running_sut(MAX_INPUT_BYTES=SMALL_INPUT_BYTES, MAX_CONNECTIONS=APPLICATION_WORKERS) as (port, _):
            with connect(port) as client:
                client.sendall(wire_request(REST_SEND, send_body("x" * SMALL_INPUT_BYTES)))
                assert_connection_closed(client)
            for _ in range(IDLE_CONNECTIONS):
                with connect(port) as client:
                    client.sendall(request(b"close"))
                    response, _ = read_response(client)
                    self.assertTrue(response.startswith(HTTP_OK))
            with connect(port) as client:
                client.sendall(request())
                client.shutdown(socket.SHUT_WR)
                response, _ = read_response(client)
                self.assertTrue(response.startswith(HTTP_OK))
                assert_connection_closed(client)


if __name__ == "__main__":
    unittest.main()
