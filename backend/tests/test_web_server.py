import pytest
import datetime
from app_state import AppState, ConnectionEntry
from web.server import create_app


@pytest.fixture
def state():
    return AppState()


@pytest.fixture
def client(state):
    app = create_app(state)
    app.config['TESTING'] = True
    return app.test_client(), state


def test_get_index_returns_200(client):
    c, _ = client
    response = c.get('/')
    assert response.status_code == 200


def test_get_index_shows_default_config(client):
    c, state = client
    response = c.get('/')
    body = response.data.decode()
    assert str(state.config.sleep_min)    in body
    assert str(state.config.idle_min)     in body
    assert str(state.config.max_acq)      in body
    assert str(state.config.cooldown_sec) in body


def test_post_config_updates_state(client):
    c, state = client
    response = c.post('/config', data={
        'sleep_min':    '120',
        'idle_min':     '10',
        'max_acq':      '3',
        'cooldown_sec': '2',
    })
    assert response.status_code == 303
    assert state.config.sleep_min    == 120
    assert state.config.idle_min     == 10
    assert state.config.max_acq      == 3
    assert state.config.cooldown_sec == 2


def test_post_config_rejects_non_integer(client):
    c, state = client
    original = state.config.sleep_min
    response = c.post('/config', data={
        'sleep_min':    'abc',
        'idle_min':     '10',
        'max_acq':      '3',
        'cooldown_sec': '2',
    })
    assert response.status_code == 400
    assert state.config.sleep_min == original


def test_post_config_rejects_zero(client):
    c, state = client
    original = state.config.sleep_min
    response = c.post('/config', data={
        'sleep_min':    '0',
        'idle_min':     '10',
        'max_acq':      '3',
        'cooldown_sec': '2',
    })
    assert response.status_code == 400
    assert state.config.sleep_min == original


def test_get_index_shows_connection_history(client):
    c, state = client
    state.connections.append(ConnectionEntry(
        ip='192.168.1.5',
        timestamp=datetime.datetime(2026, 5, 20, 14, 0, 0),
        n_samples=100,
        battery_mv=3750,
    ))
    response = c.get('/')
    body = response.data.decode()
    assert '192.168.1.5' in body
    assert '<td>100</td>' in body
    assert '<td>3750</td>' in body
