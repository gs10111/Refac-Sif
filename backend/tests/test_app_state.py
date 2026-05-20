import threading
from app_state import AppState, DeviceConfig, ConnectionEntry
import datetime


def test_default_config_matches_settings():
    from config.settings import (
        DEFAULT_SLEEP_MIN, DEFAULT_IDLE_MIN,
        DEFAULT_MAX_ACQ, DEFAULT_COOLDOWN_SEC,
    )
    state = AppState()
    assert state.config.sleep_min    == DEFAULT_SLEEP_MIN
    assert state.config.idle_min     == DEFAULT_IDLE_MIN
    assert state.config.max_acq      == DEFAULT_MAX_ACQ
    assert state.config.cooldown_sec == DEFAULT_COOLDOWN_SEC


def test_config_fields_are_mutable():
    state = AppState()
    with state.lock:
        state.config.sleep_min = 99
    assert state.config.sleep_min == 99


def test_connections_starts_empty():
    state = AppState()
    assert len(state.connections) == 0


def test_connections_append_and_order():
    state = AppState()
    entry = ConnectionEntry(
        ip='10.0.0.1',
        timestamp=datetime.datetime.now(),
        n_samples=10,
        battery_mv=3800,
    )
    with state.lock:
        state.connections.append(entry)
    assert len(state.connections) == 1
    assert state.connections[0].ip == '10.0.0.1'
    assert state.connections[0].n_samples == 10


def test_connections_max_50():
    state = AppState()
    for i in range(60):
        with state.lock:
            state.connections.append(ConnectionEntry(
                ip=f'10.0.0.{i}',
                timestamp=datetime.datetime.now(),
                n_samples=i,
                battery_mv=3800,
            ))
    assert len(state.connections) == 50


def test_lock_is_threading_lock():
    state = AppState()
    assert isinstance(state.lock, type(threading.Lock()))
