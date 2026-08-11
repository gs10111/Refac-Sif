"""The capture the server already writes to CSV is also published to MQTT.

Acceptance criterion: what the sensors send reaches ThingsBoard, and the CSV
that exists today keeps being written exactly as before — the publisher is an
addition to the save path, never a gate in front of it.

No network here: the publisher is injected, and the Drive copy is stubbed the
same way the CSV tests stub it.
"""
import datetime
import importlib
import os

import pytest

import config.settings
from server import tcp_server

BATTERY_MV = 4100
ROWS = [
    [10, 1, 2, 3, 4, 5, 6, 7, BATTERY_MV],
    [30, 8, 9, 10, 11, 12, 13, 14, BATTERY_MV],
]
WHEN = datetime.datetime(2026, 8, 11, 18, 30, 0)


class RecordingPublisher:
    """Records every publish call and answers with a chosen reason."""

    def __init__(self, reason=None):
        self.calls = []
        self._reason = reason

    def publish(self, device_ip, samples, battery_mv, anchor_dt):
        self.calls.append((device_ip, samples, battery_mv, anchor_dt))
        return self._reason


@pytest.fixture(autouse=True)
def drain_queue():
    while not tcp_server.data_queue.empty():
        tcp_server.data_queue.get_nowait()
    yield
    while not tcp_server.data_queue.empty():
        tcp_server.data_queue.get_nowait()


def run_save_data_once(tmp_path, monkeypatch, publisher=None, rows=ROWS):
    monkeypatch.chdir(tmp_path)
    monkeypatch.setattr(tcp_server.subprocess, 'run', lambda *a, **k: None)

    tcp_server.data_queue.put(('10.0.0.7', WHEN, rows))
    tcp_server.data_queue.put(None)
    tcp_server.save_data(publisher)

    return tmp_path / f"10.0.0.7_{WHEN.strftime('%Y%m%d_%H%M%S')}.csv"


def test_the_burst_is_published_with_the_device_ip_and_the_capture_instant(tmp_path, monkeypatch):
    publisher = RecordingPublisher()

    run_save_data_once(tmp_path, monkeypatch, publisher)

    assert len(publisher.calls) == 1
    device_ip, samples, battery_mv, anchor_dt = publisher.calls[0]
    assert device_ip == '10.0.0.7'
    assert samples == ROWS
    assert anchor_dt == WHEN


def test_the_battery_comes_from_the_row_the_server_already_appended(tmp_path, monkeypatch):
    """It is constant across the burst, so it travels in the summary, not per
    sample — and it is already sitting in column 8 of every row."""
    publisher = RecordingPublisher()

    run_save_data_once(tmp_path, monkeypatch, publisher)

    assert publisher.calls[0][2] == BATTERY_MV


def test_the_csv_is_written_whether_or_not_anything_is_published(tmp_path, monkeypatch):
    """The CSV is the record of last resort: a broker that is down, a burst the
    publisher refuses, or no publisher at all must not cost the file."""
    written = run_save_data_once(tmp_path, monkeypatch, RecordingPublisher(reason='recusado'))
    assert written.exists()

    another = run_save_data_once(tmp_path, monkeypatch, None)
    assert another.exists()


def test_a_capture_with_no_rows_is_never_published(tmp_path, monkeypatch):
    publisher = RecordingPublisher()

    run_save_data_once(tmp_path, monkeypatch, publisher, rows=[])

    assert publisher.calls == []


def test_a_publisher_that_raises_does_not_take_the_save_thread_down(tmp_path, monkeypatch):
    """The worker drains the queue for every later capture too. Dying on one
    broker error would silently stop every CSV from that moment on."""
    class Exploding:
        def publish(self, *args):
            raise RuntimeError('broker exploded')

    written = run_save_data_once(tmp_path, monkeypatch, Exploding())

    assert written.exists()


# --------------------------------------------------------------------------
# Configuration — fail-closed
# --------------------------------------------------------------------------

@pytest.fixture
def reload_settings():
    yield lambda: importlib.reload(config.settings)
    for key in ('SIF_MQTT_ENABLED', 'SIF_MQTT_HOST', 'SIF_MQTT_PORT'):
        os.environ.pop(key, None)
    importlib.reload(config.settings)


def test_publishing_is_off_until_it_is_asked_for(monkeypatch, reload_settings):
    monkeypatch.delenv('SIF_MQTT_ENABLED', raising=False)

    assert reload_settings().MQTT_ENABLED is False


def test_asking_for_publishing_without_a_broker_stops_the_boot(monkeypatch, reload_settings):
    """Fail-closed: starting anyway would run a server that looks configured for
    ThingsBoard and quietly publishes nowhere."""
    monkeypatch.setenv('SIF_MQTT_ENABLED', '1')
    monkeypatch.delenv('SIF_MQTT_HOST', raising=False)

    with pytest.raises(ValueError):
        reload_settings()


def test_a_broker_port_that_is_not_a_port_stops_the_boot(monkeypatch, reload_settings):
    monkeypatch.setenv('SIF_MQTT_ENABLED', '1')
    monkeypatch.setenv('SIF_MQTT_HOST', '127.0.0.1')
    monkeypatch.setenv('SIF_MQTT_PORT', 'mil-oitocentos')

    with pytest.raises(ValueError):
        reload_settings()


def test_a_configured_broker_is_read_whole(monkeypatch, reload_settings):
    monkeypatch.setenv('SIF_MQTT_ENABLED', '1')
    monkeypatch.setenv('SIF_MQTT_HOST', '10.0.0.2')
    monkeypatch.setenv('SIF_MQTT_PORT', '1884')

    settings = reload_settings()

    assert (settings.MQTT_ENABLED, settings.MQTT_HOST, settings.MQTT_PORT) == (True, '10.0.0.2', 1884)
