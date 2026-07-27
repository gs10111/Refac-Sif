import csv
import logging
import socket
import struct
import subprocess
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

# The CSV header the production servers write (pyFiles/win_server.py:45).
# Spelled out on purpose: comparing against protocol.packet.CSV_COLUMNS would
# only prove the writer agrees with the constant, not that either is right.
ORIGINAL_CSV_HEADER = [
    'timestamp', 'x_data', 'x_gyro', 'y_data', 'y_gyro', 'z_data', 'z_gyro',
    'temp', 'battery_voltage',
]


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


def exchange(state, ip='10.0.0.1', n_samples=1):
    """Run one complete connection against state and return the mock socket."""
    conn = MagicMock()
    conn.recv.side_effect = make_recv_sequence(n_samples=n_samples)
    handle_client(conn, (ip, 5000), state)
    return conn


def sent_config(conn):
    """Unpack the 10-byte ServerConfig the server wrote to the socket."""
    return struct.unpack('<HHHHH', conn.sendall.call_args[0][0])


def test_handle_client_sends_config_from_state():
    """Config values in AppState are used in the response to ESP32."""
    state = AppState()
    state.config.sleep_min    = 99
    state.config.idle_min     = 11
    state.config.max_acq      = 4
    state.config.cooldown_sec = 7

    conn = exchange(state)

    sleep_min, idle_min, max_acq, cooldown_sec, update = sent_config(conn)
    assert sleep_min    == 99
    assert idle_min     == 11
    assert max_acq      == 4
    assert cooldown_sec == 7
    assert update       == 0


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


# --------------------------------------------------------------------------
# CSV output — the historical corpus and tools/analysis/cliente_local_csv.py
# read these column names. (B5 / DEC-3)
# --------------------------------------------------------------------------

def run_save_data_once(tmp_path, monkeypatch, ip, timestamp, rows, copy=None):
    """Run save_data over a single queued item and return the file it wrote.

    copy replaces subprocess.run, so the test never shells out to gio and can
    simulate the Google Drive copy failing.
    """
    monkeypatch.chdir(tmp_path)
    monkeypatch.setattr(tcp_server.subprocess, 'run', copy or (lambda *a, **k: None))

    tcp_server.data_queue.put((ip, timestamp, rows))
    tcp_server.data_queue.put(None)  # sentinel — stops the worker loop
    tcp_server.save_data()

    return tmp_path / f"{ip}_{timestamp.strftime('%Y%m%d_%H%M%S')}.csv"


def test_save_data_writes_the_original_csv_header(tmp_path, monkeypatch):
    written = run_save_data_once(
        tmp_path, monkeypatch,
        '10.0.0.9', datetime.datetime(2026, 5, 20, 14, 0, 0),
        [[1, 2, 3, 4, 5, 6, 7, 8, BATTERY_MV]],
    )

    with written.open(newline='', encoding='utf-8') as f:
        header = next(csv.reader(f))

    assert header == ORIGINAL_CSV_HEADER


# --------------------------------------------------------------------------
# One-shot OTA arming on the wire (B0 / D3). The flag goes to the NEXT device
# that completes a transmission and is cleared by the claim itself.
# --------------------------------------------------------------------------

def test_handle_client_sends_update_zero_when_disarmed():
    state = AppState()

    conn = exchange(state)

    assert sent_config(conn)[4] == 0


def test_handle_client_sends_update_one_when_armed():
    state = AppState()
    state.set_ota_armed(True)

    conn = exchange(state)

    assert sent_config(conn)[4] == 1


def test_handle_client_clears_ota_after_send():
    state = AppState()
    state.set_ota_armed(True)

    exchange(state)

    assert state.ota_armed is False


def test_handle_client_second_connection_gets_update_zero():
    """Sequential, deliberately: this pins that the second caller sees a
    cleared flag. That two simultaneous claims cannot both win is a different
    property and lives in test_app_state's barrier test."""
    state = AppState()
    state.set_ota_armed(True)

    first  = exchange(state, ip='10.0.0.1')
    second = exchange(state, ip='10.0.0.2')

    assert sent_config(first)[4]  == 1
    assert sent_config(second)[4] == 0


def test_handle_client_logs_ota_sent_in_entry():
    state = AppState()
    state.set_ota_armed(True)

    exchange(state)

    assert state.connections[0].ota_sent is True


def test_handle_client_logs_ota_not_sent_when_disarmed():
    state = AppState()

    exchange(state)

    assert state.connections[0].ota_sent is False


def test_handle_client_sends_the_claimed_snapshot_not_a_later_config():
    """The response must come from the claim, not from re-reading state.config.

    A POST /config landing between the claim and the pack would otherwise ship
    a config that no operator ever saw paired with that arming.
    """
    state = AppState()
    with state.lock:
        state.config.sleep_min = 99
    real_take = state.take_config_for_send

    def take_then_config_changes():
        snapshot, ota = real_take()
        with state.lock:
            state.config.sleep_min = 1234  # operator saves right after the claim
        return snapshot, ota

    state.take_config_for_send = MagicMock(side_effect=take_then_config_changes)

    conn = exchange(state)

    assert state.take_config_for_send.call_count == 1
    assert sent_config(conn)[0] == 99


# --------------------------------------------------------------------------
# Failed delivery: the arming goes back, and the connection is still recorded.
# (B7, pulled forward from S4 — a device that took the flag is about to reboot
# into AP mode and disappear, so losing its history entry costs the most here.)
# --------------------------------------------------------------------------

def test_handle_client_rearms_ota_when_send_fails():
    """Nobody received the flag, so it must reach the next device instead."""
    state = AppState()
    state.set_ota_armed(True)

    failed = MagicMock()
    failed.recv.side_effect    = make_recv_sequence(n_samples=1)
    failed.sendall.side_effect = OSError('broken pipe')
    handle_client(failed, ('10.0.0.1', 5000), state)

    assert state.ota_armed is True
    assert sent_config(exchange(state, ip='10.0.0.2'))[4] == 1


def test_handle_client_does_not_rearm_when_send_succeeds():
    state = AppState()
    state.set_ota_armed(True)
    state.rearm_ota = MagicMock()

    exchange(state)

    assert state.rearm_ota.call_count == 0


def test_handle_client_logs_entry_when_send_fails():
    """The samples did arrive — the connection belongs in the history."""
    state = AppState()

    conn = MagicMock()
    conn.recv.side_effect    = make_recv_sequence(n_samples=2)
    conn.sendall.side_effect = OSError('broken pipe')
    handle_client(conn, ('10.0.0.4', 5000), state)

    assert len(state.connections) == 1
    assert state.connections[0].ip         == '10.0.0.4'
    assert state.connections[0].n_samples  == 2
    assert state.connections[0].battery_mv == BATTERY_MV


def test_handle_client_logs_ota_not_sent_when_send_fails():
    """The flag was claimed but never delivered — the history must not claim
    the device went to OTA, or the operator waits for an AP that never appears."""
    state = AppState()
    state.set_ota_armed(True)

    conn = MagicMock()
    conn.recv.side_effect    = make_recv_sequence(n_samples=1)
    conn.sendall.side_effect = OSError('broken pipe')
    handle_client(conn, ('10.0.0.4', 5000), state)

    assert state.connections[0].ota_sent is False


def test_save_data_writes_the_battery_in_the_last_column(tmp_path, monkeypatch):
    written = run_save_data_once(
        tmp_path, monkeypatch,
        '10.0.0.9', datetime.datetime(2026, 5, 20, 14, 0, 0),
        [[1, 2, 3, 4, 5, 6, 7, 8, BATTERY_MV]],
    )

    with written.open(newline='', encoding='utf-8') as f:
        header, row = list(csv.reader(f))

    assert len(row) == len(header)
    assert row[-1]  == str(BATTERY_MV)


# --------------------------------------------------------------------------
# A failed Google Drive copy is not a failed save. (B6)
# The gvfs mount is routinely absent and gio may not be installed at all; the
# CSV is on local disk either way and the operator must not be told otherwise.
# --------------------------------------------------------------------------

COPY_FAILURES = [
    subprocess.CalledProcessError(1, 'gio'),  # gvfs mount missing / copy refused
    FileNotFoundError('gio'),                 # gio not installed
]


def raising_copy(exc):
    def _run(*args, **kwargs):
        raise exc
    return _run


def save_with_failing_copy(tmp_path, monkeypatch, exc):
    return run_save_data_once(
        tmp_path, monkeypatch,
        '10.0.0.9', datetime.datetime(2026, 5, 20, 14, 0, 0),
        [[1, 2, 3, 4, 5, 6, 7, 8, BATTERY_MV]],
        copy=raising_copy(exc),
    )


@pytest.mark.parametrize('exc', COPY_FAILURES)
def test_save_data_keeps_the_csv_when_the_drive_copy_fails(tmp_path, monkeypatch, exc):
    written = save_with_failing_copy(tmp_path, monkeypatch, exc)

    assert written.exists()
    with written.open(newline='', encoding='utf-8') as f:
        rows = list(csv.reader(f))
    assert rows[0]     == ORIGINAL_CSV_HEADER
    assert len(rows)   == 2
    assert rows[1][-1] == str(BATTERY_MV)


@pytest.mark.parametrize('exc', COPY_FAILURES)
def test_save_data_does_not_report_a_save_failure_when_only_the_copy_failed(
        tmp_path, monkeypatch, caplog, exc):
    """The data is on disk — saying it was not is the actual bug."""
    caplog.set_level(logging.INFO)

    save_with_failing_copy(tmp_path, monkeypatch, exc)

    messages = [r.getMessage().lower() for r in caplog.records]
    assert not any('failed to save' in m for m in messages), messages


def test_save_data_does_not_copy_to_drive_when_the_local_write_fails(tmp_path, monkeypatch):
    """The other side of the `if saved` guard: no file, no copy.

    Copying a file the write never produced turns a disk failure into a second,
    more confusing error about a path that does not exist. This test is the only
    thing covering that branch — every other B6 test takes the write-succeeded
    path.
    """
    copy_calls = []

    def failing_open(*args, **kwargs):
        raise OSError('no space left on device')

    monkeypatch.chdir(tmp_path)
    # Module-scoped shadow of the builtin — safer than patching builtins.open
    # globally, which pytest and logging also use.
    monkeypatch.setattr(tcp_server, 'open', failing_open, raising=False)
    monkeypatch.setattr(tcp_server.subprocess, 'run',
                        lambda *a, **k: copy_calls.append(a))

    tcp_server.data_queue.put(
        ('10.0.0.9', datetime.datetime(2026, 5, 20, 14, 0, 0),
         [[1, 2, 3, 4, 5, 6, 7, 8, BATTERY_MV]]))
    tcp_server.data_queue.put(None)
    tcp_server.save_data()

    assert copy_calls == []


def test_save_data_reports_the_write_failure(tmp_path, monkeypatch, caplog):
    """A real local write failure still says so — that wording was never wrong,
    it was being used for the wrong event."""
    def failing_open(*args, **kwargs):
        raise OSError('no space left on device')

    caplog.set_level(logging.INFO)
    monkeypatch.chdir(tmp_path)
    monkeypatch.setattr(tcp_server, 'open', failing_open, raising=False)
    monkeypatch.setattr(tcp_server.subprocess, 'run', lambda *a, **k: None)

    tcp_server.data_queue.put(
        ('10.0.0.9', datetime.datetime(2026, 5, 20, 14, 0, 0),
         [[1, 2, 3, 4, 5, 6, 7, 8, BATTERY_MV]]))
    tcp_server.data_queue.put(None)
    tcp_server.save_data()

    errors = [r.getMessage().lower() for r in caplog.records if r.levelno >= logging.ERROR]
    assert any('failed to save' in m for m in errors), errors
    assert not any('drive' in m for m in errors), errors


@pytest.mark.parametrize('exc', COPY_FAILURES)
def test_save_data_reports_the_drive_copy_failure(tmp_path, monkeypatch, caplog, exc):
    """It still has to be reported — as the copy failing, not the save."""
    caplog.set_level(logging.INFO)

    save_with_failing_copy(tmp_path, monkeypatch, exc)

    errors = [r.getMessage().lower() for r in caplog.records if r.levelno >= logging.ERROR]
    assert any('drive' in m for m in errors), errors
