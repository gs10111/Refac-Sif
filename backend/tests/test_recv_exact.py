"""Tests for recv_exact — the framing helper the receive path depends on.

A TCP recv() may return fewer bytes than asked for, and returns b'' forever
once the peer has closed. Both facts are behind the header/battery bugs in the
original receive loop, so this helper is pinned on its own.
"""

import socket
from unittest.mock import MagicMock

import pytest

from server.tcp_server import recv_exact


def closed_peer_recv(max_calls=10):
    """recv stub that keeps returning b'' — like a socket whose peer closed.

    Raises after max_calls instead of returning b'' forever, so an
    implementation that spins fails the test rather than hanging the suite.
    """
    state = {'calls': 0}

    def _recv(size):
        state['calls'] += 1
        if state['calls'] > max_calls:
            raise AssertionError(
                f'recv called {state["calls"]} times on a closed peer — recv_exact is spinning'
            )
        return b''

    return _recv


def test_recv_exact_returns_exactly_n_bytes():
    conn = MagicMock()
    conn.recv.side_effect = [b'abcdefgh']
    assert recv_exact(conn, 8) == b'abcdefgh'


def test_recv_exact_uses_one_recv_when_everything_arrives_at_once():
    conn = MagicMock()
    conn.recv.side_effect = [b'abcd']
    recv_exact(conn, 4)
    assert conn.recv.call_count == 1
    assert conn.recv.call_args[0][0] == 4


def test_recv_exact_concatenates_one_byte_chunks():
    """The real socket behaviour the header bug was written against."""
    conn = MagicMock()
    conn.recv.side_effect = [b'a', b'b', b'c', b'd', b'e']
    assert recv_exact(conn, 5) == b'abcde'
    assert conn.recv.call_count == 5


def test_recv_exact_requests_only_the_missing_bytes():
    # DO NOT REMOVE. This is the ONLY test in the suite that pins the byte
    # count recv_exact asks for. Every handle_client test drives a MagicMock,
    # which ignores the requested size and returns the next side_effect item
    # regardless — so with this test gone, a recv_exact asking for the wrong
    # count passes all 110 tests. See docs/backend, L6.
    conn = MagicMock()
    conn.recv.side_effect = [b'abcd', b'efghij']
    recv_exact(conn, 10)
    assert conn.recv.call_args_list[0][0][0] == 10
    assert conn.recv.call_args_list[1][0][0] == 6


def test_recv_exact_raises_connection_error_when_peer_closes():
    conn = MagicMock()
    conn.recv.side_effect = closed_peer_recv()
    with pytest.raises(ConnectionError):
        recv_exact(conn, 2)


def test_recv_exact_does_not_spin_when_peer_closes():
    """B3: b'' must abort immediately, never loop at 100% CPU."""
    conn = MagicMock()
    conn.recv.side_effect = closed_peer_recv()
    with pytest.raises(ConnectionError):
        recv_exact(conn, 2)
    assert conn.recv.call_count == 1


def test_recv_exact_error_reports_partial_progress():
    conn = MagicMock()
    conn.recv.side_effect = [b'abc', b'']
    with pytest.raises(ConnectionError) as excinfo:
        recv_exact(conn, 8)
    assert '3/8' in str(excinfo.value)


def test_recv_exact_zero_bytes_returns_empty_without_calling_recv():
    """A device that sends a 0-byte payload must not make the server block."""
    conn = MagicMock()
    assert recv_exact(conn, 0) == b''
    assert conn.recv.call_count == 0


def test_recv_exact_propagates_socket_timeout():
    """Timeout policy belongs to the caller, not to the helper."""
    conn = MagicMock()
    conn.recv.side_effect = socket.timeout
    with pytest.raises(socket.timeout):
        recv_exact(conn, 4)
