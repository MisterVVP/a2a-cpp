#!/usr/bin/env python3
"""Verify that the internal diagnostics reset is not measured as HTTP traffic."""

from __future__ import annotations

import os
import subprocess
import sys

from http_persistence_test import (
    CONNECTION_CLOSE,
    HOST,
    HTTP_OK,
    SHUTDOWN_TIMEOUT_SECONDS,
    available_port,
    connect,
    read_response,
    request,
    request_graceful_shutdown,
    wait_until_ready,
)

DIAGNOSTICS_RESET_PATH = b"/_a2a/performance/reset-subscription-diagnostics"
HTTP_NO_CONTENT = b"HTTP/1.1 204"
EXPECTED_DIAGNOSTICS = (
    b"A2A_HTTP_DIAGNOSTICS accepted_connections=1 "
    b"completed_unary_operations=1 operations_per_connection=1 "
    b"finite_stream_connections=0 completed_finite_streams=0 "
    b"finite_streams_per_connection=0 "
    b"connections_reused_after_finite_stream=0"
)


def reset_request() -> bytes:
    return b"\r\n".join(
        (
            b"POST " + DIAGNOSTICS_RESET_PATH + b" HTTP/1.1",
            b"Host: localhost",
            b"Content-Length: 0",
            b"Connection: keep-alive",
            b"",
            b"",
        )
    )


def run_requests(port: int) -> None:
    with connect(port) as client:
        client.sendall(reset_request())
        reset_response, carry = read_response(client)
        assert reset_response.startswith(HTTP_NO_CONTENT)
        assert CONNECTION_CLOSE not in reset_response
        client.sendall(request(b"close"))
        measured_response, carry = read_response(client, carry)
        assert measured_response.startswith(HTTP_OK)
        assert CONNECTION_CLOSE in measured_response
        assert not carry


def main() -> int:
    sut = sys.argv[1]
    port = available_port()
    creation_flags = subprocess.CREATE_NEW_PROCESS_GROUP if sys.platform == "win32" else 0
    environment = os.environ.copy()
    environment["A2A_TCK_STORE_BACKEND"] = "inmemory"
    process = subprocess.Popen(
        [sut, f"{HOST}:{port}"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        creationflags=creation_flags,
        env=environment,
    )
    try:
        wait_until_ready(process, port)
        run_requests(port)
        request_graceful_shutdown(process)
        process.wait(timeout=SHUTDOWN_TIMEOUT_SECONDS)
        assert process.returncode == 0
        assert process.stdout is not None
        output = process.stdout.read()
        assert EXPECTED_DIAGNOSTICS in output
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=SHUTDOWN_TIMEOUT_SECONDS)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
