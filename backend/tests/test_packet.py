"""Wire-format tests for protocol/packet.py.

The server → ESP32 response is a shared contract with the firmware (C struct
ServerConfig) and with the production servers in pyFiles/. It is pinned here
byte for byte.
"""

import struct

import pytest

from protocol.packet import (
    SAMPLE_SIZE_BYTES, HEADER_SIZE_BYTES, BATTERY_SIZE_BYTES,
    SERVER_CONFIG_SIZE, SAMPLE_COLUMNS, CSV_COLUMNS,
    parse_sample, pack_server_config,
)
from config.settings import (
    DEFAULT_SLEEP_MIN, DEFAULT_IDLE_MIN, DEFAULT_MAX_ACQ,
    DEFAULT_COOLDOWN_SEC, DEFAULT_UPDATE,
)


# Exact CSV header written by the production servers
# (pyFiles/win_server.py:45 and pyFiles/server_lix_csv2.py:49, identical).
ORIGINAL_CSV_HEADER = [
    'timestamp', 'x_data', 'x_gyro', 'y_data', 'y_gyro', 'z_data', 'z_gyro',
    'temp', 'battery_voltage',
]

# struct.pack('<HHHHH', 240, 20, 5, 5, 1) — what pyFiles/win_server.py:114 emits
# with the agreed defaults and the OTA flag set.
GOLDEN_RESPONSE_UPDATE_ON = b'\xf0\x00\x14\x00\x05\x00\x05\x00\x01\x00'


# --------------------------------------------------------------------------
# Server → ESP32 response: 10 bytes, 5 × uint16 LE
# --------------------------------------------------------------------------

def test_server_config_size_is_10():
    """The response grew from 8 to 10 bytes when `update` was restored."""
    assert SERVER_CONFIG_SIZE == 10


def test_pack_server_config_returns_10_bytes():
    assert len(pack_server_config(240, 20, 5, 5, 0)) == SERVER_CONFIG_SIZE


def test_pack_server_config_field_order():
    """Field order is the original one: sleep, idle, max_acq, cooldown, update."""
    packed = pack_server_config(240, 20, 5, 5, 1)
    assert struct.unpack('<HHHHH', packed) == (240, 20, 5, 5, 1)


def test_pack_server_config_matches_win_server_golden_bytes():
    """Byte-for-byte identical to the production server's response."""
    assert pack_server_config(240, 20, 5, 5, 1) == GOLDEN_RESPONSE_UPDATE_ON


def test_pack_server_config_defaults_match_settings():
    """Packing the documented defaults reproduces the golden frame (update=0)."""
    packed = pack_server_config(
        DEFAULT_SLEEP_MIN, DEFAULT_IDLE_MIN, DEFAULT_MAX_ACQ,
        DEFAULT_COOLDOWN_SEC, DEFAULT_UPDATE,
    )
    assert packed == b'\xf0\x00\x14\x00\x05\x00\x05\x00\x00\x00'


def test_pack_server_config_requires_update_argument():
    """`update` has no default — every call site must be explicit about OTA."""
    with pytest.raises(TypeError):
        pack_server_config(240, 20, 5, 5)


@pytest.mark.parametrize('update', [2, 65535, -1])
def test_pack_server_config_rejects_update_out_of_range(update):
    """Only 0 and 1 may reach the wire; the firmware treats any non-zero as OTA."""
    with pytest.raises(ValueError):
        pack_server_config(240, 20, 5, 5, update)


# --------------------------------------------------------------------------
# CSV header — production corpus + tools/analysis/cliente_local_csv.py
# --------------------------------------------------------------------------

def test_csv_columns_match_original_header():
    """CSV header is byte-identical to the production servers' header."""
    assert CSV_COLUMNS == ORIGINAL_CSV_HEADER


def test_sample_columns_are_the_header_without_the_battery():
    assert SAMPLE_COLUMNS == ORIGINAL_CSV_HEADER[:-1]
    assert CSV_COLUMNS == SAMPLE_COLUMNS + ['battery_voltage']


# --------------------------------------------------------------------------
# Uplink frame layout (characterization — must stay byte-identical)
# --------------------------------------------------------------------------

def test_frame_size_constants():
    assert SAMPLE_SIZE_BYTES  == 18
    assert HEADER_SIZE_BYTES  == 4
    assert BATTERY_SIZE_BYTES == 2


def test_parse_sample_field_order_and_signedness():
    """18 B = uint32 timestamp + 7 × int16, little-endian, in this order."""
    raw = struct.pack('<I7h', 123456, 1, -2, 3, -4, 5, -6, 7)
    assert parse_sample(raw) == [123456, 1, -2, 3, -4, 5, -6, 7]


def test_parse_sample_returns_one_field_per_sample_column():
    raw = bytes(SAMPLE_SIZE_BYTES)
    assert len(parse_sample(raw)) == len(SAMPLE_COLUMNS)
