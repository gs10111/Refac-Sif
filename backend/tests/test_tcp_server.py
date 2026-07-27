import socket
import struct
import datetime
from unittest.mock import MagicMock

import pytest

from app_state import AppState
from protocol.packet import (
    HEADER_SIZE_BYTES, SAMPLE_SIZE_BYTES, BATTERY_SIZE_BYTES, SAMPLE_COLUMNS,
)
from config.settings import MAX_PAYLOAD_BYTES, BATTERY_INVALID
from server import tcp_server
from server.tcp_server import handle_client


BATTERY_MV = 3800


@pytest.fixture(autouse=True)
def drain_data_queue():
    """The CSV queue is a module global — keep it from leaking between tests."""
    while not tcp_server.data_queue.empty():
        tcp_server.data_queue.get_nowait()
    yield
    while not tcp_server.data_queue.empty():
        tcp_server.data_queue.get_nowait()


def queued_rows():
    """Rows handed to save_data by the last connection, or [] if nothing was queued."""
    if tcp_server.data_queue.empty():
        return []
    _ip, _timestamp, samples = tcp_server.data_queue.get_nowait()
    return samples


def make_recv_sequence(n_samples: int = 1, extra_bytes: int = 0):
    """Return list of bytes objects for conn.recv side_effect.

    extra_bytes appends a partial (sub-frame) tail to the payload, as an old
    firmware whose ring buffer size was not a multiple of SAMPLE_SIZE_BYTES
    would send.
    """
    body   = bytes(SAMPLE_SIZE_BYTES * n_samples + extra_bytes)
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


# --------------------------------------------------------------------------
# Framing: the payload boundary comes from the header, never from counting
# bytes as they arrive. (B1, B2, B8)
# --------------------------------------------------------------------------

def test_handle_client_parses_every_complete_frame():
    state = AppState()
    conn  = MagicMock()
    conn.recv.side_effect = make_recv_sequence(n_samples=3)

    handle_client(conn, ('10.0.0.1', 5000), state)

    assert state.connections[0].n_samples == 3


def test_handle_client_reads_battery_when_payload_is_not_frame_aligned():
    """B1: the battery must not be swallowed by the frame parser.

    A payload of 3 frames + 16 trailing bytes puts exactly 18 bytes in front of
    the parser once the 2 battery bytes arrive — the shape that used to produce
    a fake sample and then block until the socket timed out.
    """
    state = AppState()
    conn  = MagicMock()
    conn.recv.side_effect = make_recv_sequence(n_samples=3, extra_bytes=16)

    handle_client(conn, ('10.0.0.1', 5000), state)

    assert state.connections[0].battery_mv == BATTERY_MV


def test_handle_client_discards_trailing_partial_frame_legacy_firmware_compat():
    """Compatibility path, not live behaviour.

    DEC-1 makes our firmware send a payload that is always a multiple of 18, so
    there is no remainder on the wire. An older build with a 700000-byte ring
    sends 16 trailing bytes; they are not a sample and must be dropped.
    """
    state = AppState()
    conn  = MagicMock()
    conn.recv.side_effect = make_recv_sequence(n_samples=3, extra_bytes=16)

    handle_client(conn, ('10.0.0.1', 5000), state)

    assert state.connections[0].n_samples == 3


def test_handle_client_reassembles_split_header():
    """B2: a header split across two recv calls must still be read whole."""
    state  = AppState()
    header, body, batt = make_recv_sequence(n_samples=2)
    conn   = MagicMock()
    conn.recv.side_effect = [header[:1], header[1:], body, batt]

    handle_client(conn, ('10.0.0.1', 5000), state)

    assert state.connections[0].n_samples  == 2
    assert state.connections[0].battery_mv == BATTERY_MV


def test_handle_client_reassembles_chunked_payload():
    state  = AppState()
    header, body, batt = make_recv_sequence(n_samples=4)
    conn   = MagicMock()
    conn.recv.side_effect = [header, body[:10], body[10:25], body[25:], batt]

    handle_client(conn, ('10.0.0.1', 5000), state)

    assert state.connections[0].n_samples == 4


def test_handle_client_handles_empty_payload():
    """A device that wakes with nothing to send still gets its config."""
    state  = AppState()
    header = (0).to_bytes(HEADER_SIZE_BYTES, 'little')
    batt   = BATTERY_MV.to_bytes(BATTERY_SIZE_BYTES, 'little')
    conn   = MagicMock()
    conn.recv.side_effect = [header, batt]

    handle_client(conn, ('10.0.0.1', 5000), state)

    assert conn.sendall.call_count == 1
    assert state.connections[0].n_samples  == 0
    assert state.connections[0].battery_mv == BATTERY_MV


def test_handle_client_rejects_absurd_expected_size():
    """B8: a corrupt header must be refused before anything is allocated."""
    state  = AppState()
    conn   = MagicMock()
    conn.recv.side_effect = [(0xFFFFFFFF).to_bytes(HEADER_SIZE_BYTES, 'little')]

    handle_client(conn, ('10.0.0.1', 5000), state)

    # Exactly one recv — the header. Reading the payload was never attempted.
    assert conn.recv.call_count   == 1
    assert conn.sendall.call_count == 0
    assert len(state.connections)  == 0


def test_handle_client_rejects_payload_over_max():
    state = AppState()
    conn  = MagicMock()
    conn.recv.side_effect = [(MAX_PAYLOAD_BYTES + 1).to_bytes(HEADER_SIZE_BYTES, 'little')]

    handle_client(conn, ('10.0.0.1', 5000), state)

    assert conn.recv.call_count   == 1
    assert conn.sendall.call_count == 0


# --------------------------------------------------------------------------
# Battery degradation must match production: -1, and the config still goes out.
# (B3, DEC-4)
# --------------------------------------------------------------------------

def test_handle_client_sends_config_when_battery_is_missing():
    """The device dropped the link on the last two bytes — it still gets told
    how long to sleep, exactly as the production server does."""
    state  = AppState()
    header, body, _ = make_recv_sequence(n_samples=2)
    conn   = MagicMock()
    conn.recv.side_effect = [header, body, b'']

    handle_client(conn, ('10.0.0.1', 5000), state)

    assert conn.sendall.call_count == 1


def test_handle_client_logs_invalid_battery_when_battery_is_missing():
    state  = AppState()
    header, body, _ = make_recv_sequence(n_samples=2)
    conn   = MagicMock()
    conn.recv.side_effect = [header, body, b'']

    handle_client(conn, ('10.0.0.1', 5000), state)

    assert state.connections[0].battery_mv == BATTERY_INVALID


def test_handle_client_sends_config_when_battery_times_out():
    state  = AppState()
    header, body, _ = make_recv_sequence(n_samples=2)
    conn   = MagicMock()
    conn.recv.side_effect = [header, body, socket.timeout()]

    handle_client(conn, ('10.0.0.1', 5000), state)

    assert conn.sendall.call_count == 1
    assert state.connections[0].battery_mv == BATTERY_INVALID


# --------------------------------------------------------------------------
# CSV rows always carry the battery column. (B4)
# --------------------------------------------------------------------------

def test_handle_client_queues_rows_with_the_battery_column():
    state = AppState()
    conn  = MagicMock()
    conn.recv.side_effect = make_recv_sequence(n_samples=3)

    handle_client(conn, ('10.0.0.1', 5000), state)

    rows = queued_rows()
    assert len(rows) == 3
    assert all(len(row) == len(SAMPLE_COLUMNS) + 1 for row in rows)
    assert all(row[-1] == BATTERY_MV for row in rows)


def test_handle_client_queues_rows_with_invalid_battery_when_battery_is_missing():
    """No ragged CSV: the column is there even when the reading is not."""
    state  = AppState()
    header, body, _ = make_recv_sequence(n_samples=2)
    conn   = MagicMock()
    conn.recv.side_effect = [header, body, b'']

    handle_client(conn, ('10.0.0.1', 5000), state)

    rows = queued_rows()
    assert len(rows) == 2
    assert all(len(row) == len(SAMPLE_COLUMNS) + 1 for row in rows)
    assert all(row[-1] == BATTERY_INVALID for row in rows)


def test_handle_client_queues_nothing_when_payload_is_truncated():
    """Peer vanished mid-payload: no config, no history entry, nothing saved —
    the same data outcome as the production server, minus the crashed worker."""
    state  = AppState()
    header, body, _ = make_recv_sequence(n_samples=4)
    conn   = MagicMock()
    conn.recv.side_effect = [header, body[:20], b'']

    handle_client(conn, ('10.0.0.1', 5000), state)

    assert conn.sendall.call_count == 0
    assert len(state.connections)  == 0
    assert queued_rows()           == []
