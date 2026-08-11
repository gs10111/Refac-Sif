"""The server chooses the acquisition rate; the sensor adopts it next cycle.

Acceptance criterion: the operator picks a rate between 12.5 and 200 Hz in an
environment variable of the Linux server, and the device adopts it on the
following cycle without being reflashed.

The rate travels as the sixth uint16 of the config response — the ODR nibble
from the ICM-42688-P datasheet, not the frequency in Hz — so the firmware can
write it straight into ACCEL_CONFIG0/GYRO_CONFIG0 without a table of its own.

No network here: the packing is exercised directly, and the one test that goes
through a connection uses the mocked socket of test_tcp_server.py.
"""
import importlib
import os
import struct

import pytest

import config.settings
from protocol.packet import (
    SERVER_CONFIG_SIZE, SAMPLING_CODES, SAMPLING_CODE_NO_CHANGE,
    pack_server_config, sampling_code_from_hz,
)


@pytest.fixture
def reload_settings():
    """Re-run config.settings so os.getenv is read again, and restore it after."""
    yield lambda: importlib.reload(config.settings)
    os.environ.pop('SIF_SAMPLING_HZ', None)
    importlib.reload(config.settings)


def test_the_config_response_is_twelve_bytes():
    """Five policy fields plus the rate. The device reads a fixed-size frame."""
    assert SERVER_CONFIG_SIZE == 12
    assert len(pack_server_config(240, 20, 5, 5, 0, 9)) == 12


def test_the_rate_is_the_last_field_on_the_wire():
    packed = pack_server_config(240, 20, 5, 5, 1, 7)

    assert struct.unpack('<HHHHHH', packed) == (240, 20, 5, 5, 1, 7)


@pytest.mark.parametrize('hz, code', [
    ('200', 7), ('100', 8), ('50', 9), ('25', 10), ('12.5', 11),
])
def test_every_supported_rate_maps_to_its_datasheet_nibble(hz, code):
    """The five rates where accelerometer AND gyroscope both run: codes 12-14
    are Reserved for the gyroscope on this part, and 500 Hz is out of scope."""
    assert SAMPLING_CODES[hz] == code
    assert sampling_code_from_hz(hz) == code


def test_a_rate_the_part_cannot_do_is_refused_by_name():
    with pytest.raises(ValueError) as excinfo:
        sampling_code_from_hz('150')

    assert '150' in str(excinfo.value)
    assert '12.5' in str(excinfo.value)   # the message lists what is accepted


def test_an_integer_rate_written_as_a_float_is_the_same_rate():
    """`50.0` and `50` are the same operator intent; only the table key differs."""
    assert sampling_code_from_hz('50.0') == SAMPLING_CODES['50']


def test_a_server_with_no_opinion_sends_the_no_change_code():
    """0 is not a valid ODR nibble on this part, so the firmware keeps its rate."""
    assert SAMPLING_CODE_NO_CHANGE == 0

    packed = pack_server_config(240, 20, 5, 5, 0, SAMPLING_CODE_NO_CHANGE)

    assert struct.unpack('<HHHHHH', packed)[5] == 0


def test_an_invented_code_never_reaches_the_wire():
    """Refusing here is what keeps a nibble the part treats as Reserved from
    being written into ACCEL_CONFIG0 by a device that trusts the server."""
    with pytest.raises(ValueError):
        pack_server_config(240, 20, 5, 5, 0, 13)


def test_the_rate_comes_from_the_environment(monkeypatch, reload_settings):
    monkeypatch.setenv('SIF_SAMPLING_HZ', '200')

    assert reload_settings().SAMPLING_CODE == 7


def test_the_rate_defaults_to_the_fifty_hertz_the_fleet_runs_today(monkeypatch, reload_settings):
    monkeypatch.delenv('SIF_SAMPLING_HZ', raising=False)

    assert reload_settings().SAMPLING_CODE == SAMPLING_CODES['50']


def test_an_unusable_rate_stops_the_boot(monkeypatch, reload_settings):
    """Fail-closed: falling back to 50 Hz would hand the operator a fleet
    sampling at a rate they did not ask for and cannot see."""
    monkeypatch.setenv('SIF_SAMPLING_HZ', '150')

    with pytest.raises(ValueError):
        reload_settings()
