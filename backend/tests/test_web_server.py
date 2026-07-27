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


def test_post_config_rejects_negative(client):
    c, state = client
    original = state.config.sleep_min
    response = c.post('/config', data={
        'sleep_min':    '-5',
        'idle_min':     '10',
        'max_acq':      '3',
        'cooldown_sec': '2',
    })
    assert response.status_code == 400
    assert state.config.sleep_min == original


# --------------------------------------------------------------------------
# Form validation: which field, and why. A four-field form that answers "one
# of these is wrong" makes the operator guess, and the fastest wrong guess is
# to change a field that was already right.
# --------------------------------------------------------------------------

FIELD_NAMES = ['sleep_min', 'idle_min', 'max_acq', 'cooldown_sec']
VALID_FORM  = {'sleep_min': '240', 'idle_min': '20', 'max_acq': '5', 'cooldown_sec': '5'}

# Every field travels as a uint16 in the 10-byte response, so this is the
# protocol's limit, not a policy we invented.
UINT16_MAX = 65535


def form(**overrides):
    data = dict(VALID_FORM)
    data.update(overrides)
    return data


def config_tuple(state):
    return (state.config.sleep_min, state.config.idle_min,
            state.config.max_acq,   state.config.cooldown_sec)


def test_post_config_rejects_zero_max_acq(client):
    """max_acq=0 means "collect nothing" — the device would obey it correctly
    and the whole plant would sleep 240 min between doing nothing."""
    c, state = client
    before = config_tuple(state)

    response = c.post('/config', data=form(max_acq='0'))

    assert response.status_code == 400
    assert config_tuple(state) == before


@pytest.mark.parametrize('field', FIELD_NAMES)
def test_post_config_rejects_zero_in_any_field(client, field):
    c, state = client
    before = config_tuple(state)

    response = c.post('/config', data=form(**{field: '0'}))

    assert response.status_code == 400
    assert config_tuple(state) == before


def test_post_config_error_message_names_the_offending_field(client):
    c, _ = client

    response = c.post('/config', data=form(max_acq='0'))
    body = response.data.decode()

    assert 'max_acq' in body
    # A message that lists every field on any error is the generic message
    # again, with extra steps.
    assert 'sleep_min'    not in body
    assert 'idle_min'     not in body
    assert 'cooldown_sec' not in body


def test_post_config_error_message_lists_every_offender(client):
    """Two bad fields, one round trip — in fixed form order."""
    c, _ = client

    response = c.post('/config', data=form(sleep_min='0', max_acq='70000'))
    body = response.data.decode()

    assert 'sleep_min' in body
    assert 'max_acq'   in body
    assert 'idle_min'  not in body
    assert body.index('sleep_min') < body.index('max_acq')


@pytest.mark.parametrize('field', FIELD_NAMES)
def test_post_config_rejects_value_above_uint16(client, field):
    """Above 65535 the value cannot be packed. Today it is accepted and stored,
    and then every device connection dies inside pack_server_config — the page
    shows a config that was never applied to anything."""
    c, state = client
    before = config_tuple(state)

    response = c.post('/config', data=form(**{field: str(UINT16_MAX + 1)}))

    assert response.status_code == 400
    assert config_tuple(state) == before


def test_post_config_accepts_uint16_maximum(client):
    """The bound is inclusive — 65535 fits the field."""
    c, state = client

    response = c.post('/config', data=form(sleep_min=str(UINT16_MAX)))

    assert response.status_code == 303
    assert state.config.sleep_min == UINT16_MAX


def test_post_config_error_message_mentions_the_protocol_limit(client):
    c, _ = client

    response = c.post('/config', data=form(sleep_min=str(UINT16_MAX + 1)))
    body = response.data.decode()

    assert 'sleep_min' in body
    assert str(UINT16_MAX) in body
    assert 'idle_min' not in body


def test_get_index_shows_connection_history(client):
    c, state = client
    with state.lock:
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
