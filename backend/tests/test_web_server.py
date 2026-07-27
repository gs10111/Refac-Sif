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


def test_post_config_accepts_one_as_the_minimum(client):
    """The lower bound is inclusive. max_acq=1 and cooldown_sec=1 are ordinary
    operator values, and nothing pinned that `<= 0` was not `<= 1`."""
    c, state = client

    response = c.post('/config', data=form(
        sleep_min='1', idle_min='1', max_acq='1', cooldown_sec='1'))

    assert response.status_code == 303
    assert config_tuple(state) == (1, 1, 1, 1)


def test_post_config_error_message_mentions_the_protocol_limit(client):
    c, _ = client

    response = c.post('/config', data=form(sleep_min=str(UINT16_MAX + 1)))
    body = response.data.decode()

    assert 'sleep_min' in body
    assert str(UINT16_MAX) in body
    assert 'idle_min' not in body


# --------------------------------------------------------------------------
# One-shot OTA arming (DEC-3/DEC-5). Separate route from the config form, and
# the page must show the armed state BEFORE any device has connected.
# --------------------------------------------------------------------------

def test_get_index_shows_ota_disarmed_by_default(client):
    c, _ = client

    body = c.get('/').data.decode()

    assert 'ARMAR OTA'  in body
    assert 'OTA ARMADO' not in body


def test_get_index_shows_armed_badge_before_any_connection(client):
    """Arming is visible immediately — waiting for a device to prove it armed
    is how an operator arms it twice or drives to the plant to check."""
    c, state = client
    state.set_ota_armed(True)

    body = c.get('/').data.decode()

    assert len(state.connections) == 0
    assert 'OTA ARMADO' in body
    assert 'DESARMAR'   in body


def test_get_index_armed_page_differs_from_disarmed(client):
    """The property behind the requirement: armed must not render identically
    to disarmed, whatever the markup happens to be."""
    c, state = client

    disarmed = c.get('/').data
    state.set_ota_armed(True)
    armed    = c.get('/').data

    assert armed != disarmed


def test_post_ota_arms(client):
    c, state = client

    response = c.post('/ota', data={'armed': '1'})

    assert response.status_code == 303
    assert state.ota_armed is True


def test_post_ota_disarms(client):
    c, state = client
    state.set_ota_armed(True)

    response = c.post('/ota', data={'armed': '0'})

    assert response.status_code == 303
    assert state.ota_armed is False


def test_post_ota_rejects_invalid_value(client):
    c, state = client

    response = c.post('/ota', data={'armed': 'maybe'})

    assert response.status_code == 400
    assert state.ota_armed is False


def test_post_ota_rejects_missing_value(client):
    c, state = client
    state.set_ota_armed(True)

    response = c.post('/ota', data={})

    assert response.status_code == 400
    assert state.ota_armed is True


def test_post_ota_missing_field_message_says_so(client):
    c, _ = client

    body = c.post('/ota', data={}).data.decode()

    assert 'campo ausente' in body
    assert 'nao e um valor valido' not in body


def test_post_ota_invalid_value_message_quotes_it(client):
    c, _ = client

    body = c.post('/ota', data={'armed': 'maybe'}).data.decode()

    assert '"maybe"' in body
    assert 'campo ausente' not in body


def test_index_is_never_cached(client):
    """A back-button render showing a stale arming is what costs the operator
    their trust in the page."""
    c, _ = client

    response = c.get('/')

    assert response.headers['Cache-Control'] == 'no-store'


def test_post_ota_does_not_change_config(client):
    """DEC-5: the two forms are independent."""
    c, state = client
    before = config_tuple(state)

    response = c.post('/ota', data={'armed': '1'})

    assert response.status_code == 303
    assert config_tuple(state) == before


def test_post_config_does_not_change_ota(client):
    """DEC-5 from the other side: saving config must never disarm."""
    c, state = client
    state.set_ota_armed(True)

    response = c.post('/config', data=form(sleep_min='120'))

    assert response.status_code == 303
    assert state.ota_armed is True


def test_post_ota_on_a_fresh_server_renders_with_empty_history(client):
    """Most likely first use of the whole feature: arm the flag on a server
    that has never seen a device. The empty-history path must still render."""
    c, state = client

    assert c.post('/ota', data={'armed': '1'}).status_code == 303
    response = c.get('/')
    body = response.data.decode()

    assert response.status_code == 200
    assert len(state.connections) == 0
    assert 'OTA ARMADO' in body


# --------------------------------------------------------------------------
# History: which device took the flag, readable at a glance.
# --------------------------------------------------------------------------

def add_entry(state, ip='192.168.1.5', ota_sent=False, minute=0):
    with state.lock:
        state.connections.append(ConnectionEntry(
            ip=ip,
            timestamp=datetime.datetime(2026, 5, 20, 14, minute, 0),
            n_samples=100,
            battery_mv=3750,
            ota_sent=ota_sent,
        ))


def test_index_shows_the_last_device_that_took_the_ota(client):
    """"Which sensor did I just send into AP mode" is the question the operator
    has while standing there. Pure render over the history — no inference."""
    c, state = client
    add_entry(state, ip='192.168.1.7', ota_sent=True, minute=3)

    body = c.get('/').data.decode()

    assert 'Ultimo OTA enviado: 192.168.1.7' in body
    assert '20/05 14:03' in body


def test_index_shows_no_last_ota_when_none_was_ever_sent(client):
    """BORROWED SAFETY: an empty response body satisfies this too. What makes it
    mean something is test_index_shows_the_last_device_that_took_the_ota, which
    asserts the same string IS rendered when an entry took the flag."""
    c, state = client
    add_entry(state, ip='192.168.1.5', ota_sent=False)

    body = c.get('/').data.decode()

    assert 'Ultimo OTA enviado' not in body


def test_index_shows_the_most_recent_ota_when_several(client):
    c, state = client
    add_entry(state, ip='192.168.1.5', ota_sent=True, minute=1)
    add_entry(state, ip='192.168.1.9', ota_sent=True, minute=5)

    body = c.get('/').data.decode()

    # Anchored on the full line: both IPs appear in the history table below.
    assert 'Ultimo OTA enviado: 192.168.1.9' in body
    assert 'Ultimo OTA enviado: 192.168.1.5' not in body


def test_index_last_ota_survives_later_normal_connections(client):
    """It is the last connection that TOOK the flag, not the last connection."""
    c, state = client
    add_entry(state, ip='192.168.1.7', ota_sent=True, minute=3)
    add_entry(state, ip='192.168.1.8', ota_sent=False, minute=9)

    body = c.get('/').data.decode()

    assert 'Ultimo OTA enviado: 192.168.1.7' in body


def test_history_marks_the_entry_that_took_the_ota(client):
    """Row styling AND a text badge: colour alone fails a colour-blind operator
    and a black-and-white print of the page."""
    c, state = client
    add_entry(state, ota_sent=True)

    body = c.get('/').data.decode()

    assert '<tr class="ota-row">' in body
    assert 'class="ota-badge"'    in body
    assert '>OTA<'                in body


def test_history_does_not_mark_a_normal_entry(client):
    """BORROWED SAFETY: an empty body, or a template that stopped rendering rows
    at all, satisfies both assertions. test_history_marks_the_entry_that_took_the_ota
    is what proves the markup appears when it should."""
    c, state = client
    add_entry(state, ota_sent=False)

    body = c.get('/').data.decode()

    # Anchored on the rendered row, not on the class name: the stylesheet
    # defines .ota-row / .ota-badge on every page, armed or not.
    assert '<tr class="ota-row">' not in body
    assert 'class="ota-badge"'    not in body


def test_history_empty_row_spans_every_column(client):
    c, _ = client

    body = c.get('/').data.decode()

    assert 'colspan="5"' in body


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
