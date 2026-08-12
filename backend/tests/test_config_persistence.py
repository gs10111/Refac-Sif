"""What the operator saves on the page is what the next device receives.

Acceptance criterion, in three parts: the page shows the rate the fleet is
running; the rate is changed from the page, in a route of its own; and the whole
configuration survives a restart of the server, taking effect on the next device
that transmits rather than on the next boot of the process.

No network and no server here: the store is a temporary file and the page is
exercised through Flask's test client.
"""
import datetime

import pytest

from app_state import AppState, ConnectionEntry
from protocol.packet import SAMPLING_CODES
from store.config_store import DEFAULTS, init_db, read_config, write_config
from web.server import create_app


@pytest.fixture
def db(tmp_path):
    path = str(tmp_path / 'sif.db')
    init_db(path)
    return path


@pytest.fixture
def client(db):
    state = AppState(db_path=db)
    app = create_app(state)
    app.config['TESTING'] = True
    return app.test_client(), state, db


# --------------------------------------------------------------------------
# Read per connection, not once at startup
# --------------------------------------------------------------------------

def test_a_device_connecting_now_gets_what_was_saved_a_moment_ago(db):
    """The whole point of reading per connection: no restart between the save
    and the next device."""
    state = AppState(db_path=db)

    write_config(db, sleep_min=15, sampling_code=SAMPLING_CODES['200'])
    config, _ = state.take_config_for_send()

    assert config.sleep_min == 15
    assert config.sampling_code == SAMPLING_CODES['200']


def test_the_configuration_outlives_the_process(db):
    """A second AppState on the same file is what a restart looks like."""
    AppState(db_path=db).update_config(idle_min=9)

    assert AppState(db_path=db).take_config_for_send()[0].idle_min == 9


def test_without_a_database_the_configuration_is_ephemeral_as_it_always_was(tmp_path):
    """Running with no file is still supported, and still means the values go
    back to the defaults on restart — stated, not implied."""
    state = AppState()

    state.update_config(sleep_min=15)

    assert state.take_config_for_send()[0].sleep_min == 15
    assert AppState().take_config_for_send()[0].sleep_min == DEFAULTS['sleep_min']


# --------------------------------------------------------------------------
# The page shows the rate
# --------------------------------------------------------------------------

def test_the_page_shows_the_rate_the_fleet_is_running(client):
    c, _, db = client
    write_config(db, sampling_code=SAMPLING_CODES['25'])

    body = c.get('/').data.decode()

    assert '25 Hz' in body


# --------------------------------------------------------------------------
# The rate is changed in a route of its own
# --------------------------------------------------------------------------

def test_the_rate_is_saved_from_the_page(client):
    c, _, db = client

    response = c.post('/sampling', data={'sampling_hz': '200'})

    assert response.status_code == 303
    assert read_config(db)['sampling_code'] == SAMPLING_CODES['200']


def test_a_rate_the_part_cannot_run_is_refused_by_name(client):
    """Refused at the edge, before it can reach the store or a device."""
    c, _, db = client

    response = c.post('/sampling', data={'sampling_hz': '150'})

    assert response.status_code == 400
    assert b'150' in response.data
    assert read_config(db)['sampling_code'] == DEFAULTS['sampling_code']


def test_a_missing_rate_field_is_refused(client):
    c, _, db = client

    assert c.post('/sampling', data={}).status_code == 400
    assert read_config(db)['sampling_code'] == DEFAULTS['sampling_code']


def test_saving_the_duty_cycle_never_changes_the_rate(client):
    """Separate routes for the same reason OTA has its own: one careless save of
    an unrelated field must not re-rate every sensor in the plant."""
    c, _, db = client
    write_config(db, sampling_code=SAMPLING_CODES['200'])

    c.post('/config', data={'sleep_min': '11', 'idle_min': '12',
                            'max_acq': '13', 'cooldown_sec': '14'})

    config = read_config(db)
    assert config['sleep_min'] == 11
    assert config['sampling_code'] == SAMPLING_CODES['200']


def test_saving_the_rate_never_changes_the_duty_cycle(client):
    c, _, db = client
    write_config(db, sleep_min=11)

    c.post('/sampling', data={'sampling_hz': '12.5'})

    config = read_config(db)
    assert config['sampling_code'] == SAMPLING_CODES['12.5']
    assert config['sleep_min'] == 11


def test_a_rejected_duty_cycle_still_writes_nothing_at_all(client):
    """The rule the form already had, now that the values reach a file."""
    c, _, db = client

    response = c.post('/config', data={'sleep_min': '0', 'idle_min': '12',
                                       'max_acq': '13', 'cooldown_sec': '14'})

    assert response.status_code == 400
    assert read_config(db) == DEFAULTS


def test_a_database_that_cannot_be_written_answers_instead_of_crashing(client, monkeypatch):
    """Disk full, file gone read-only, database locked past the timeout: the
    operator gets a page that says what happened, not a bare 500 that reads as
    "the server is broken" while the sensors keep transmitting fine."""
    import sqlite3

    from store import config_store

    def refuse(*args, **kwargs):
        raise sqlite3.OperationalError('attempt to write a readonly database')

    c, _, _ = client
    monkeypatch.setattr(config_store, 'write_config', refuse)

    rate = c.post('/sampling', data={'sampling_hz': '200'})
    duty = c.post('/config', data={'sleep_min': '11', 'idle_min': '12',
                                   'max_acq': '13', 'cooldown_sec': '14'})

    assert rate.status_code == 503
    assert duty.status_code == 503
    assert b'readonly' in rate.data or b'salvar' in rate.data.lower()


def test_the_history_and_the_arming_stay_in_memory(client):
    """Only the configuration was moved to disk. Saying otherwise would promise
    an operator that the connection history survives a restart."""
    c, state, _ = client
    state.connections.append(ConnectionEntry(
        ip='10.0.0.7', timestamp=datetime.datetime(2026, 8, 12, 10, 0),
        n_samples=1, battery_mv=4100))
    state.set_ota_armed(True)

    assert AppState(db_path=state.db_path).connections == \
        type(state.connections)(maxlen=state.connections.maxlen)
    assert AppState(db_path=state.db_path).ota_armed is False
