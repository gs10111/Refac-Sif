"""When a device connection dies, the server says how far it got.

Acceptance criterion: a connection that times out or breaks tells whoever reads
the log — or the page — how many bytes actually arrived and how many were
expected. Without that, "Timeout from 192.168.1.118" cannot distinguish a device
that sent nothing from one that sent almost everything, and the only way to find
out was a packet capture.

No network here except the loopback pair the existing socket test already uses.
"""
import socket
import struct
import threading

import pytest

from app_state import AppState
from protocol.packet import SERVER_CONFIG_SIZE
from server.tcp_server import handle_client, recv_exact


@pytest.fixture
def socket_pair():
    """A real loopback pair, for the same reason test_integration_socket has one:
    only a real socket can be half-fed and then left silent."""
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.bind(('127.0.0.1', 0))
    listener.listen(1)
    client = socket.create_connection(listener.getsockname(), timeout=5)
    conn, _ = listener.accept()
    yield client, conn
    for s in (client, conn, listener):
        try:
            s.close()
        except OSError:
            pass


# --------------------------------------------------------------------------
# recv_exact reports its own progress
# --------------------------------------------------------------------------

def test_a_read_that_times_out_says_how_much_it_had(socket_pair):
    """The number is the whole point: 0 of 4 is a device that never spoke, 4530
    of 4536 is a link that dropped at the end. They need different fixes."""
    client, conn = socket_pair
    conn.settimeout(0.2)
    client.sendall(b'ab')

    with pytest.raises(socket.timeout) as excinfo:
        recv_exact(conn, 4)

    assert '2' in str(excinfo.value)
    assert '4' in str(excinfo.value)


def test_a_peer_that_closes_early_says_how_much_it_had(socket_pair):
    client, conn = socket_pair
    conn.settimeout(1.0)
    client.sendall(b'abc')
    client.close()

    with pytest.raises(ConnectionError) as excinfo:
        recv_exact(conn, 10)

    assert '3' in str(excinfo.value)
    assert '10' in str(excinfo.value)


def test_a_complete_read_is_unchanged(socket_pair):
    client, conn = socket_pair
    conn.settimeout(1.0)
    client.sendall(b'abcd')

    assert recv_exact(conn, 4) == b'abcd'


# --------------------------------------------------------------------------
# What the operator reads when a device gives up mid-transfer
# --------------------------------------------------------------------------

def run_handle_client(conn, state):
    thread = threading.Thread(target=handle_client, args=(conn, ('127.0.0.1', 1234), state))
    thread.start()
    return thread


def test_a_device_that_connects_and_says_nothing_is_reported_as_such(socket_pair, caplog):
    """Exactly the case from the bench: the connection is accepted, the device
    believes it wrote, and not one byte arrives."""
    client, conn = socket_pair
    state = AppState()

    with caplog.at_level('WARNING'):
        thread = run_handle_client(conn, state)
        thread.join(timeout=15)

    assert not thread.is_alive()
    message = ' '.join(record.getMessage() for record in caplog.records)
    assert '0' in message                     # bytes received
    assert 'cabecalho' in message.lower() or 'header' in message.lower()


def test_a_device_that_sends_a_header_and_stops_is_told_apart(socket_pair, caplog):
    """A header that promises 4536 bytes followed by silence must not read the
    same as silence from the start."""
    client, conn = socket_pair
    state = AppState()
    client.sendall(struct.pack('<I', 4536))

    with caplog.at_level('WARNING'):
        thread = run_handle_client(conn, state)
        thread.join(timeout=15)

    assert not thread.is_alive()
    message = ' '.join(record.getMessage() for record in caplog.records)
    assert '4536' in message
    assert state.connections[0].complete is False


def test_a_complete_exchange_still_answers_with_the_config(socket_pair):
    """The diagnostics must not have cost the happy path."""
    client, conn = socket_pair
    state = AppState()
    payload = struct.pack('<I7h', 1000, 1, 2, 3, 4, 5, 6, 7)
    client.sendall(struct.pack('<I', len(payload)) + payload + struct.pack('<H', 4100))

    thread = run_handle_client(conn, state)
    response = client.recv(SERVER_CONFIG_SIZE)
    thread.join(timeout=15)

    assert len(response) == SERVER_CONFIG_SIZE
    assert state.connections[0].n_samples == 1
