# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

"""Temporary TCK client header/metadata overrides.

The conformance SUT intentionally advertises a required extension. Current TCK
clients do not yet propagate required extension support from the Agent Card into
ordinary HTTP or gRPC calls, so the SDK CI injects it for positive conformance
cases while leaving the negative missing-extension tests untouched.
"""

from __future__ import annotations

import os
from typing import Any, Callable, Iterable

EXTENSION_HEADER = os.environ.get("A2A_TCK_CLIENT_REQUIRED_EXTENSIONS", "")


def _should_add_extension_header() -> bool:
    current_test = os.environ.get("PYTEST_CURRENT_TEST", "")
    return "test_missing_required_extension_returns_error" not in current_test


def _headers_with_extensions(headers: Any) -> dict[str, str]:
    merged = dict(headers or {})
    if not _should_add_extension_header():
        return merged
    if not any(name.lower() == "a2a-extensions" for name in merged):
        merged["A2A-Extensions"] = EXTENSION_HEADER
    return merged


def _metadata_with_extensions(metadata: Iterable[tuple[str, str]] | None) -> list[tuple[str, str]]:
    merged = list(metadata or [])
    if not _should_add_extension_header():
        return merged
    if not any(str(name).lower() == "a2a-extensions" for name, _ in merged):
        merged.append(("a2a-extensions", EXTENSION_HEADER))
    return merged


def _request_with_extensions(original: Callable[..., Any]) -> Callable[..., Any]:
    def wrapper(self: Any, *args: Any, **kwargs: Any) -> Any:
        kwargs["headers"] = _headers_with_extensions(kwargs.get("headers"))
        return original(self, *args, **kwargs)

    return wrapper


def _send_with_extensions(original: Callable[..., Any]) -> Callable[..., Any]:
    def wrapper(self: Any, request: Any, *args: Any, **kwargs: Any) -> Any:
        if _should_add_extension_header() and "a2a-extensions" not in request.headers:
            request.headers["A2A-Extensions"] = EXTENSION_HEADER
        return original(self, request, *args, **kwargs)

    return wrapper


class _GrpcCallableWithExtensions:
    def __init__(self, original: Any) -> None:
        self._original = original

    def __call__(self, *args: Any, **kwargs: Any) -> Any:
        kwargs["metadata"] = _metadata_with_extensions(kwargs.get("metadata"))
        return self._original(*args, **kwargs)

    def __getattr__(self, name: str) -> Any:
        return getattr(self._original, name)


def _grpc_channel_method_with_extensions(original: Callable[..., Any]) -> Callable[..., Any]:
    def wrapper(self: Any, *args: Any, **kwargs: Any) -> _GrpcCallableWithExtensions:
        return _GrpcCallableWithExtensions(original(self, *args, **kwargs))

    return wrapper


def _patch_httpx() -> None:
    try:
        import httpx
    except Exception:  # pragma: no cover - only active after TCK dependencies are installed.
        return

    httpx.Client.request = _request_with_extensions(httpx.Client.request)
    httpx.AsyncClient.request = _request_with_extensions(httpx.AsyncClient.request)
    httpx.Client.build_request = _request_with_extensions(httpx.Client.build_request)
    httpx.AsyncClient.build_request = _request_with_extensions(httpx.AsyncClient.build_request)
    httpx.Client.send = _send_with_extensions(httpx.Client.send)
    httpx.AsyncClient.send = _send_with_extensions(httpx.AsyncClient.send)


def _patch_grpc() -> None:
    try:
        import grpc
    except Exception:  # pragma: no cover - only active after TCK dependencies are installed.
        return

    aio_channel = getattr(getattr(grpc, "aio", None), "Channel", None)
    for channel_type in (grpc.Channel, aio_channel):
        if channel_type is None:
            continue
        channel_type.unary_unary = _grpc_channel_method_with_extensions(channel_type.unary_unary)
        channel_type.unary_stream = _grpc_channel_method_with_extensions(channel_type.unary_stream)
        channel_type.stream_unary = _grpc_channel_method_with_extensions(channel_type.stream_unary)
        channel_type.stream_stream = _grpc_channel_method_with_extensions(channel_type.stream_stream)


if EXTENSION_HEADER:
    _patch_httpx()
    _patch_grpc()
