import socket
import struct
import datetime
from unittest.mock import MagicMock

import pytest

from app_state import AppState
from protocol.packet import (
    HEADER_SIZE_BYTES, SAMPLE_SIZE_BYTES, BATTERY_SIZE_BYTES,
)
from server.tcp_server import handle_client


BATTERY_MV = 3800


def make_recv_sequence(n_samples: int = 1):
    """Return list of bytes objects for conn.recv side_effect."""
    body   = bytes(SAMPLE_SIZE_BYTES * n_samples)
    header = len(body).to_bytes(HEADER_SIZE_BYTES, 'little')
    batt   = BATTERY_MV.to_bytes(BATTERY_SIZE_BYTES, 'little')
    return [header, body, batt]


def test_handle_client_sends_config_from_state():
    """Config values in AppState are used in the response to ESP32."""
    state = AppState()
    state.config.sleep_min    = 99
    state.config.idle_min     = 11
    state.config.max_acq      = 4
    state.config.cooldown_sec = 7

    conn = MagicMock()
    conn.recv.side_effect = make_recv_sequence(n_samples=1)

    handle_client(conn, ('10.0.0.1', 5000), state)

    sent = conn.sendall.call_args[0][0]
    sleep_min, idle_min, max_acq, cooldown_sec = struct.unpack('<HHHH', sent)
    assert sleep_min    == 99
    assert idle_min     == 11
    assert max_acq      == 4
    assert cooldown_sec == 7


def test_handle_client_logs_connection_to_state():
    """After a successful exchange, a ConnectionEntry is added to state."""
    state = AppState()

    conn = MagicMock()
    conn.recv.side_effect = make_recv_sequence(n_samples=2)

    handle_client(conn, ('10.0.0.2', 5001), state)

    assert len(state.connections) == 1
    entry = state.connections[0]
    assert entry.ip         == '10.0.0.2'
    assert entry.n_samples  == 2
    assert entry.battery_mv == BATTERY_MV
    assert isinstance(entry.timestamp, datetime.datetime)


def test_handle_client_does_not_log_on_timeout():
    """A socket timeout produces no ConnectionEntry."""
    state = AppState()

    conn = MagicMock()
    conn.recv.side_effect = socket.timeout

    handle_client(conn, ('10.0.0.3', 5002), state)

    assert len(state.connections) == 0
