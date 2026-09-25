#!/usr/bin/env python3
"""Deterministic MCP resource server shared by the production tutorials."""

from __future__ import annotations

import argparse
from pathlib import Path

from mcp.server.fastmcp import FastMCP
from starlette.requests import Request
from starlette.responses import JSONResponse, Response

JOB_FIXTURE_SET = "job_application"
SUPPORT_FIXTURE_SET = "customer_support"
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8090


def read_fixture(root: Path, filename: str) -> str:
    """Read one allow-listed fixture and reject missing or empty data."""
    contents = (root / filename).read_text(encoding="utf-8")
    if not contents:
        raise ValueError(f"MCP fixture is empty: {filename}")
    return contents


def create_server(fixture_set: str, fixture_root: Path, host: str, port: int) -> FastMCP:
    """Create an official-SDK MCP server for one tutorial fixture set."""
    server = FastMCP(
        "a2a-cpp tutorial resource server",
        host=host,
        port=port,
        json_response=True,
    )

    @server.custom_route("/health", methods=["GET"])
    async def health(_: Request) -> Response:
        return JSONResponse({"status": "ok"})

    if fixture_set == JOB_FIXTURE_SET:
        resume = read_fixture(fixture_root, "resume.txt")

        @server.resource("resume://candidate/alex", name="Alex Morgan resume", mime_type="text/plain")
        def candidate_resume() -> str:
            return resume

    elif fixture_set == SUPPORT_FIXTURE_SET:
        billing = read_fixture(fixture_root, "billing_currency_ticket.txt")
        mfa = read_fixture(fixture_root, "mfa_ticket.txt")
        export_ticket = read_fixture(fixture_root, "export_ticket.txt")

        @server.resource(
            "ticket://northstar/billing-currency", name="Billing currency ticket", mime_type="text/plain"
        )
        def billing_ticket() -> str:
            return billing

        @server.resource("ticket://northstar/mfa-device", name="MFA device ticket", mime_type="text/plain")
        def mfa_ticket() -> str:
            return mfa

        @server.resource("ticket://northstar/export-delay", name="Export delay ticket", mime_type="text/plain")
        def delayed_export_ticket() -> str:
            return export_ticket

    else:
        raise ValueError(f"unsupported fixture set: {fixture_set}")

    return server


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture-set", choices=(JOB_FIXTURE_SET, SUPPORT_FIXTURE_SET), required=True)
    parser.add_argument("--fixture-root", type=Path, required=True)
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    server = create_server(args.fixture_set, args.fixture_root, args.host, args.port)
    server.run(transport="streamable-http")


if __name__ == "__main__":
    main()
