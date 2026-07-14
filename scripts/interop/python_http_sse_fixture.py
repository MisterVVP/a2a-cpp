#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

import argparse
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

A2A_VERSION = "1.0"
REST_PATH = "/a2a/message:stream"
JSON_RPC_PATH = "/rpc"


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

    def do_POST(self) -> None:
        content_length = int(self.headers.get("Content-Length", "0"))
        raw_body = self.rfile.read(content_length)
        try:
            request = json.loads(raw_body)
        except json.JSONDecodeError:
            self.send_error(400, "invalid JSON")
            return

        if self.headers.get("Accept") != "text/event-stream":
            self.send_error(406, "missing SSE accept header")
            return
        if self.headers.get("A2A-Version") != A2A_VERSION:
            self.send_error(400, "missing A2A version")
            return

        if self.path == REST_PATH:
            frames = self._rest_frames(request)
        elif self.path == JSON_RPC_PATH:
            frames = self._json_rpc_frames(request)
        else:
            self.send_error(404)
            return

        if frames is None:
            return

        self.send_response(200)
        self.send_header("A2A-Version", A2A_VERSION)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()

        for frame in frames:
            for part in fragment(frame.encode("utf-8")):
                self.wfile.write(part)
                self.wfile.flush()
                time.sleep(0.002)
        self.close_connection = True

    def _rest_frames(self, request: object) -> list[str] | None:
        if not isinstance(request, dict) or "message" not in request:
            self.send_error(400, "missing message")
            return None
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
        return [f"data: {json.dumps(working, separators=(',', ':'))}\n\n", f"data: {json.dumps(completed, separators=(',', ':'))}\n\n"]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", required=True, type=int)
    args = parser.parse_args()
    server = ThreadingHTTPServer((args.host, args.port), FixtureHandler)
    server.serve_forever()


if __name__ == "__main__":
    main()
