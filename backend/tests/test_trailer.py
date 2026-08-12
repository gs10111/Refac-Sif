"""The end of an uplink says which firmware ran and what rate it achieved.

Acceptance criterion: the server records, per capture, the firmware version the
sensor is running and the rate it actually reached — not the rate it was told to
run. Until now the message ended with two bytes of battery, so the only thing
the server knew about a device was what it had asked of it.

A fleet is reflashed one sensor at a time, so both trailers have to work at
once: exactly 7 bytes from the new firmware, exactly 2 from the old. Any other
length means the framing broke, and is refused rather than guessed at.
"""
import socket
import struct
import threading

import pytest

from app_state import AppState
from protocol.packet import (
    LEGACY_TRAILER_SIZE, TRAILER_SIZE, parse_trailer,
)
from server.tcp_server import handle_client

SAMPLE = struct.pack('<I7h', 1000, 1, 2, 3, 4, 5, 6, 7)


def trailer(battery=4100, major=1, minor=1, patch=0, hz=199):
    return struct.pack('<H3BH', battery, major, minor, patch, hz)


@pytest.fixture
def socket_pair():
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
# Reading the trailer
# --------------------------------------------------------------------------

def test_the_sizes_are_the_two_the_wire_allows():
    assert (TRAILER_SIZE, LEGACY_TRAILER_SIZE) == (7, 2)


def test_a_full_trailer_gives_battery_version_and_measured_rate():
    parsed = parse_trailer(trailer(battery=4100, major=1, minor=1, patch=0, hz=199))

    assert parsed.battery_mv == 4100
    assert parsed.firmware == '1.1.0'
    assert parsed.effective_hz == 199
    assert parsed.legacy is False


def test_the_old_two_byte_trailer_still_reads_as_a_battery():
    """A fleet is reflashed one sensor at a time; the old ones keep working."""
    parsed = parse_trailer(struct.pack('<H', 3900))

    assert parsed.battery_mv == 3900
    assert parsed.firmware is None
    assert parsed.effective_hz is None
    assert parsed.legacy is True


@pytest.mark.parametrize('size', [1, 3, 5, 6, 8, 18])
def test_any_other_length_is_a_broken_frame_not_a_missing_field(size):
    """Tolerating a 5-byte trailer would mean reading a shifted payload as
    battery and version — numbers that look like data and are not."""
    with pytest.raises(ValueError) as excinfo:
        parse_trailer(b'\x00' * size)

    assert str(size) in str(excinfo.value)


def test_the_battery_sits_where_it_always_did():
    """The first two bytes are unchanged, which is what lets a server that only
    ever read a battery keep reading one."""
    full = trailer(battery=4100)

    assert struct.unpack('<H', full[:2])[0] == 4100


# --------------------------------------------------------------------------
# What the server records
# --------------------------------------------------------------------------

def run_handle_client(conn, state):
    thread = threading.Thread(target=handle_client, args=(conn, ('127.0.0.1', 1234), state))
    thread.start()
    return thread


def test_a_capture_records_the_firmware_and_the_rate_it_achieved(socket_pair):
    client, conn = socket_pair
    state = AppState()
    client.sendall(struct.pack('<I', len(SAMPLE)) + SAMPLE + trailer(hz=199))

    thread = run_handle_client(conn, state)
    client.recv(12)
    thread.join(timeout=15)

    entry = state.connections[0]
    assert entry.battery_mv == 4100
    assert entry.firmware == '1.1.0'
    assert entry.effective_hz == 199


def test_a_sensor_on_the_old_firmware_still_gets_its_config(socket_pair):
    """It reports no version and no rate, and nothing else about it changes."""
    client, conn = socket_pair
    state = AppState()
    client.sendall(struct.pack('<I', len(SAMPLE)) + SAMPLE + struct.pack('<H', 3900))

    thread = run_handle_client(conn, state)
    response = client.recv(12)
    thread.join(timeout=15)

    entry = state.connections[0]
    assert len(response) == 12
    assert entry.battery_mv == 3900
    assert entry.firmware is None
    assert entry.effective_hz is None


def test_a_trailer_that_never_arrives_costs_the_device_nothing(socket_pair):
    """Losing the trailer must not cost the config — same degradation the
    production server has always had for a missing battery."""
    client, conn = socket_pair
    state = AppState()
    client.sendall(struct.pack('<I', len(SAMPLE)) + SAMPLE)

    thread = run_handle_client(conn, state)
    response = client.recv(12)
    thread.join(timeout=20)

    assert len(response) == 12
    assert state.connections[0].battery_mv == -1


def test_the_page_shows_the_firmware_and_the_measured_rate(socket_pair):
    """The operator asked "is it really at 200 Hz?" — this is where they read it
    without opening a CSV."""
    from web.server import create_app

    client, conn = socket_pair
    state = AppState()
    client.sendall(struct.pack('<I', len(SAMPLE)) + SAMPLE + trailer(hz=199))
    thread = run_handle_client(conn, state)
    client.recv(12)
    thread.join(timeout=15)

    app = create_app(state)
    app.config['TESTING'] = True
    body = app.test_client().get('/').data.decode()

    assert '1.1.0' in body
    assert '199' in body
