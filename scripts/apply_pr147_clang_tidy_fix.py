#!/usr/bin/env python3

from pathlib import Path

path = Path("src/client/http_json_transport.cpp")
content = path.read_text(encoding="utf-8")

content = content.replace(
    '#include <thread>\n',
    '#include <thread>\n#include <utility>\n',
    1,
)

anchor = '''void NotifyErrorAndStop(StreamHandle::State& state, StreamObserver& observer, const core::Error& error) {
  observer