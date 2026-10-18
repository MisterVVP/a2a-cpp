#!/usr/bin/env python3
"""Check security launcher port isolation and reservation cleanup."""

import unittest
from unittest.mock import MagicMock, patch

import run_security_scenarios as launcher

BASE_PORT = 40000
PROXY_PORT = 41000
SOCKETS_PER_ATTEMPT = 3
REAL_RESERVATIONS = 8


def sockets(base=BASE_PORT):
    reserved = [MagicMock() for _ in range(SOCKETS_PER_ATTEMPT)]
    for descriptor, port in zip(reserved, (base, base + 1, PROXY_PORT)):
        descriptor.__enter__.return_value = descriptor
        descriptor.getsockname.return_value = (launcher.LOOPBACK_HOST, port)
    return reserved


class SecurityPortsTest(unittest.TestCase):
    def test_proxy_selection_holds_both_sut_reservations(self):
        reserved = sockets()

        def bind_proxy(_address):
            for descriptor in reserved[:2]:
                descriptor.bind.assert_called_once()
                descriptor.__exit__.assert_not_called()

        reserved[2].bind.side_effect = bind_proxy
        with patch.object(launcher.socket, "socket", side_effect=reserved):
            self.assertEqual(launcher.reserve_security_ports(), (BASE_PORT, PROXY_PORT))
        for descriptor in reserved:
            descriptor.__exit__.assert_called_once()

    def test_adjacent_port_conflict_releases_sockets_and_retries(self):
        rejected = sockets()
        accepted = sockets(BASE_PORT + SOCKETS_PER_ATTEMPT)
        rejected[1].bind.side_effect = OSError()
        with patch.object(launcher.socket, "socket", side_effect=rejected + accepted):
            self.assertEqual(launcher.reserve_security_ports(),
                             (BASE_PORT + SOCKETS_PER_ATTEMPT, PROXY_PORT))
        for descriptor in rejected + accepted:
            descriptor.__exit__.assert_called_once()

    def test_real_socket_reservations_never_overlap(self):
        for _ in range(REAL_RESERVATIONS):
            http, tls = launcher.reserve_security_ports()
            self.assertNotIn(tls, (http, http + 1))


if __name__ == "__main__":
    unittest.main()
