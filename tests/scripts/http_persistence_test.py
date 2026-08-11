#!/usr/bin/env python3
"""Deterministic persistent-connection checks against the C++ TCK SUT."""

from __future__ import annotations

import socket
import subprocess
import sys
import threading
import time

HOST = "127.0.0.1"
HEADER_END = b"\r\n\r\n"
HTTP_OK = b"HTTP/1.1 200"
CONNECTION_CLOSE = b"Connection: close"
MALFORMED_REQUEST = b"POST /rpc HTTP/1.1\r\nMalformed\r\n\r\n"
RPC_BODY = b'{"jsonrpc":"2.0","id":"reuse-test","method":"tasks/list","params":{}}'
RPC_PATH = b"/rpc"
STARTUP_TIMEOUT_SECONDS = 20.0
SOCKET_TIMEOUT_SECONDS = 5.0
SHUTDOWN_TIMEOUT_SECONDS = 10.0
FRAGMENT_SIZE = 3
CONCURRENT_CONNECTIONS = 4


def available_port() -> int:
    with socket.socket() as listener:
        listener.bind((HOST, 0))
        port = listener.getsockname()[1]
    if port >= 65534:
        return available_port()
    return port


def request(connection: bytes = b"keep-alive") -> bytes:
    headers = [
        b"POST " + RPC_PATH + b" HTTP/1.1",
        b"Host: localhost",
        b"Content-Type: application/json",
        b"Content-Length: " + str(len(RPC_BODY)).encode("ascii"),
        b"Connection: " + connection,
    ]
    return b"\r\n".join(headers) + HEADER_END + RPC_BODY


def read_response(client: socket.socket, buffered: bytes = b"") -> tuple[bytes, bytes]:
    while HEADER_END not in buffered:
        buffered += client.recv(4096)
    headers, buffered = buffered.split(HEADER_END, 1)
    content_length = 0
    for line in headers.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            content_length = int(line.split(b":", 1)[1].strip())
    while len(buffered) < content_length:
        buffered += client.recv(4096)
    return headers + HEADER_END + buffered[:content_length], buffered[content_length:]


def connect(port: int) -> socket.socket:
    client = socket.create_connection((HOST, port), timeout=SOCKET_TIMEOUT_SECONDS)
    client.settimeout(SOCKET_TIMEOUT_SECONDS)
    return client


def wait_until_ready(process: subprocess.Popen[bytes], port: int) -> None:
    deadline = time.monotonic() + STARTUP_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError("TCK SUT exited during startup")
        try:
            with connect(port):
                return
        except OSError:
            time.sleep(0.05)
    raise AssertionError("TCK SUT did not become ready")


def check_sequential_and_pipelined(port: int) -> None:
    with connect(port) as client:
        client.sendall(request() + request(b"close"))
        first, carry = read_response(client)
        second, carry = read_response(client, carry)
        assert first.startswith(HTTP_OK)
        assert CONNECTION_CLOSE not in first
        assert second.startswith(HTTP_OK)
        assert CONNECTION_CLOSE in second
        assert not carry
        assert client.recv(1) == b""


def check_fragmented_request(port: int) -> None:
    payload = request(b"close")
    with connect(port) as client:
        for offset in range(0, len(payload), FRAGMENT_SIZE):
            client.sendall(payload[offset : offset + FRAGMENT_SIZE])
        response, _ = read_response(client)
        assert response.startswith(HTTP_OK)


def check_malformed_request_closes_connection(port: int) -> None:
    with connect(port) as client:
        client.sendall(MALFORMED_REQUEST)
        try:
            assert client.recv(1) == b""
        except ConnectionResetError:
            pass


def check_concurrent_connections(port: int) -> None:
    failures: list[BaseException] = []

    def worker() -> None:
        try:
            with connect(port) as client:
                client.sendall(request(b"close"))
                response, _ = read_response(client)
                assert response.startswith(HTTP_OK)
        except BaseException as error:  # Preserve assertion details from worker threads.
            failures.append(error)

    threads = [threading.Thread(target=worker) for _ in range(CONCURRENT_CONNECTIONS)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    if failures:
        raise failures[0]


def main() -> int:
    sut = sys.argv[1]
    port = available_port()
    process = subprocess.Popen([sut, f"{HOST}:{port}"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    idle_client: socket.socket | None = None
    try:
        wait_until_ready(process, port)
        check_sequential_and_pipelined(port)
        check_fragmented_request(port)
        check_malformed_request_closes_connection(port)
        check_concurrent_connections(port)
        idle_client = connect(port)
        process.terminate()
        process.wait(timeout=SHUTDOWN_TIMEOUT_SECONDS)
        try:
            assert idle_client.recv(1) == b""
        except ConnectionResetError:
            pass
        assert process.returncode == 0
    finally:
        if idle_client is not None:
            idle_client.close()
        if process.poll() is None:
            process.kill()
            process.wait(timeout=SHUTDOWN_TIMEOUT_SECONDS)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
