#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

import argparse
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

A2A_VERSION = "1.0"
REST_SEND_PATH = "/a2a/message:stream"
REST_SUBSCRIBE_PATH = "/a2a/tasks/fixture-task:subscribe"
JSON_RPC_PATH = "/rpc"
CANCEL_DELAY_SECONDS = 5


def fragment(payload: bytes) -> list[bytes]:
    cuts = (1, 7, 19, 37, 61)
    result: list[bytes] = []
    start = 0
    for end in cuts:
        if end < len(payload):
            result.append(payload[start:end])
            start = end
    result.append(payload[start:])
    return [part for part in result if part]


class FixtureHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format_string: str, *args: object) -> None:
        del format_string, args

    def do_GET(self) -> None:
        if urlsplit(self.path).path != REST_SUBSCRIBE_PATH:
            self.send_error(404)
            return
        if not self._validate_headers():
            return
        self._write_stream(self._rest_frames())

    def do_POST(self) -> None:
        content_length = int(self.headers.get("Content-Length", "0"))
        raw_body = self.rfile.read(content_length)
        try:
            request = json.loads(raw_body)
        except json.JSONDecodeError:
            self.send_error(400, "invalid JSON")
            return
        if not self._validate_headers():
            return

        path = urlsplit(self.path).path
        if path == REST_SEND_PATH:
            if not isinstance(request, dict) or "message" not in request:
                self.send_error(400, "missing message")
                return
            frames = self._rest_frames()
        elif path == JSON_RPC_PATH:
            frames = self._json_rpc_frames(request)
            if frames is None:
                return
        else:
            self.send_error(404)
            return
        self._write_stream(frames)

    def _validate_headers(self) -> bool:
        if self.headers.get("Accept") != "text/event-stream":
            self.send_error(406, "missing SSE accept header")
            return False
        if self.headers.get("A2A-Version") != A2A_VERSION:
            self.send_error(400, "missing A2A version")
            return False
        return True

    def _write_stream(self, frames: list[str]) -> None:
        self.send_response(200)
        self.send_header("A2A-Version", A2A_VERSION)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()

        selected_frames = frames[:1] if self.headers.get("X-Fixture-Mode") == "cancel" else frames
        try:
            for frame in selected_frames:
                for part in fragment(frame.encode("utf-8")):
                    self.wfile.write(part)
                    self.wfile.flush()
                    time.sleep(0.002)
            if self.headers.get("X-Fixture-Mode") == "cancel":
                time.sleep(CANCEL_DELAY_SECONDS)
        except (BrokenPipeError, ConnectionResetError):
            pass
        self.close_connection = True

    @staticmethod
    def _rest_frames() -> list[str]:
        return [
            'data: {"statusUpdate":{"taskId":"fixture-task","status":{"state":"TASK_STATE_WORKING"}}}\n\n',
            'data: {"statusUpdate":{"taskId":"fixture-task","status":{"state":"TASK_STATE_COMPLETED"},"final":true}}\n\n',
        ]

    def _json_rpc_frames(self, request: object) -> list[str] | None:
        if not isinstance(request, dict):
            self.send_error(400, "invalid JSON-RPC request")
            return None
        if request.get("jsonrpc") != "2.0" or not request.get("id") or "params" not in request:
            self.send_error(400, "invalid JSON-RPC envelope")
            return None
        request_id = request["id"]
        working = {
            "jsonrpc": "2.0",
            "id": request_id,
            "result": {
                "statusUpdate": {
                    "taskId": "fixture-task",
                    "status": {"state": "TASK_STATE_WORKING"},
                }
            },
        }
        completed = {
            "jsonrpc": "2.0",
            "id": request_id,
            "result": {
                "statusUpdate": {
                    "taskId": "fixture-task",
                    "status": {"state": "TASK_STATE_COMPLETED"},
                    "final": True,
                }
            },
        }
        return [
            f"data: {json.dumps(working, separators=(',', ':'))}\n\n",
            f"data: {json.dumps(completed, separators=(',', ':'))}\n\n",
        ]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", required=True, type=int)
    args = parser.parse_args()
    server = ThreadingHTTPServer((args.host, args.port), FixtureHandler)
    server.serve_forever()


if __name__ == "__main__":
    main()
