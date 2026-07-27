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


# --------------------------------------------------------------------------
# One-shot OTA arming (D3): the flag is sent to the NEXT device that completes
# a transmission and is cleared in the same atomic operation.
# --------------------------------------------------------------------------

def test_ota_armed_defaults_false():
    assert AppState().ota_armed is False


def test_ota_armed_default_matches_settings():
    from config.settings import DEFAULT_UPDATE
    assert AppState().ota_armed is bool(DEFAULT_UPDATE)


def test_set_ota_armed_arms_and_disarms():
    state = AppState()
    state.set_ota_armed(True)
    assert state.ota_armed is True
    state.set_ota_armed(False)
    assert state.ota_armed is False


def test_take_config_for_send_returns_false_when_disarmed():
    state = AppState()
    _, ota = state.take_config_for_send()
    assert ota is False


def test_take_config_for_send_returns_current_config_values():
    state = AppState()
    with state.lock:
        state.config.sleep_min    = 111
        state.config.idle_min     = 22
        state.config.max_acq      = 3
        state.config.cooldown_sec = 4
    config, _ = state.take_config_for_send()
    assert isinstance(config, DeviceConfig)
    assert (config.sleep_min, config.idle_min, config.max_acq, config.cooldown_sec) \
        == (111, 22, 3, 4)


def test_take_config_for_send_returns_config_snapshot():
    """The returned config is a copy — later mutations must not leak into it."""
    from config.settings import DEFAULT_SLEEP_MIN
    state = AppState()
    config, _ = state.take_config_for_send()
    with state.lock:
        state.config.sleep_min = 999
    assert config.sleep_min == DEFAULT_SLEEP_MIN


def test_take_config_for_send_claims_once_then_clears():
    state = AppState()
    state.set_ota_armed(True)

    _, first  = state.take_config_for_send()
    _, second = state.take_config_for_send()

    assert first  is True
    assert second is False
    assert state.ota_armed is False


def test_take_config_for_send_is_atomic_under_concurrency():
    """With 20 devices claiming at once, exactly one may get the OTA flag."""
    state = AppState()
    state.set_ota_armed(True)

    n_workers    = 20
    barrier      = threading.Barrier(n_workers)
    results      = []
    errors       = []
    results_lock = threading.Lock()

    def worker():
        try:
            barrier.wait()  # every thread claims at the same instant — no timing luck
            _, ota = state.take_config_for_send()
        except Exception as e:            # noqa: BLE001 — reported via assert below
            with results_lock:
                errors.append(repr(e))
            return
        with results_lock:
            results.append(ota)

    threads = [threading.Thread(target=worker) for _ in range(n_workers)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    assert errors == [], f'workers raised: {errors}'
    assert len(results) == n_workers
    assert results.count(True) == 1


def test_rearm_ota_gives_arming_back_after_failed_send():
    """Send failed — nobody received the flag, so the arming is returned."""
    state = AppState()
    state.set_ota_armed(True)

    _, ota = state.take_config_for_send()
    assert ota is True
    state.rearm_ota()

    assert state.ota_armed is True
    _, next_ota = state.take_config_for_send()
    assert next_ota is True


def test_rearm_ota_twice_does_not_double_arm():
    """Arming is a boolean, not a counter: two rearms still yield one claim."""
    state = AppState()
    state.set_ota_armed(True)
    state.take_config_for_send()

    state.rearm_ota()
    state.rearm_ota()

    _, first  = state.take_config_for_send()
    _, second = state.take_config_for_send()
    assert first  is True
    assert second is False


def test_connection_entry_ota_sent_defaults_false():
    entry = ConnectionEntry(
        ip='10.0.0.1',
        timestamp=datetime.datetime.now(),
        n_samples=10,
        battery_mv=3800,
    )
    assert entry.ota_sent is False


def test_connection_entry_records_ota_sent():
    entry = ConnectionEntry(
        ip='10.0.0.1',
        timestamp=datetime.datetime.now(),
        n_samples=10,
        battery_mv=3800,
        ota_sent=True,
    )
    assert entry.ota_sent is True
