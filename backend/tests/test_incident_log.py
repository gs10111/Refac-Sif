"""Whoever changes the configuration from the browser can see what failed.

Acceptance criterion: an operator working on the page — the only instrument they
have — sees the failures the server hits, instead of a page that looks healthy
while the terminal nobody is watching fills with errors.

Every warning and error the server already logs is captured; nothing had to be
instrumented twice. The log lives in memory, like the connection history.
"""
import logging

import pytest

from app_state import AppState, INCIDENT_LEVELS
from observability.incident_log import install_incident_log, uninstall_incident_log
from web.server import create_app


@pytest.fixture
def state_with_log():
    state = AppState()
    handler = install_incident_log(state)
    yield state
    uninstall_incident_log(handler)


@pytest.fixture
def client(state_with_log):
    app = create_app(state_with_log)
    app.config['TESTING'] = True
    return app.test_client(), state_with_log


# --------------------------------------------------------------------------
# What is captured
# --------------------------------------------------------------------------

def test_an_error_the_server_logs_becomes_a_visible_incident(state_with_log):
    """The thirteen existing logging.error/warning calls are the point: they are
    captured where they already are, not duplicated at every call site."""
    logging.error('Config not delivered to 10.0.0.7: [Errno 32] Broken pipe')

    incident = state_with_log.recent_incidents()[0]

    assert incident.level == 'ERROR'
    assert '10.0.0.7' in incident.message
    assert incident.timestamp is not None


def test_a_warning_is_captured_too(state_with_log):
    """A burst the publisher refused is a warning, and it is exactly the kind of
    failure that used to be invisible: the capture is on disk, nothing crashed,
    and nothing reached ThingsBoard."""
    logging.warning('Burst from 10.0.0.7 not published: timestamps implausiveis')

    assert state_with_log.recent_incidents()[0].level == 'WARNING'


def test_routine_progress_is_not_an_incident(state_with_log):
    """INFO is the server saying it is working. Mixed in, it would bury the
    three lines that matter under a hundred that do not.

    The root logger is lowered on purpose: the running server calls
    basicConfig(level=INFO), so INFO records DO reach the handlers there. Left at
    the default WARNING this test would pass without the handler filtering
    anything — it would be testing the logger, not the code under test.
    """
    root = logging.getLogger()
    previous = root.level
    root.setLevel(logging.INFO)
    try:
        logging.info('Saved 10.0.0.7_20260812_100000.csv')
    finally:
        root.setLevel(previous)

    assert state_with_log.recent_incidents() == []


def test_the_newest_incident_comes_first(state_with_log):
    logging.warning('primeiro')
    logging.warning('segundo')

    assert state_with_log.recent_incidents()[0].message == 'segundo'


def test_a_message_with_formatting_arguments_is_rendered(state_with_log):
    """logging defers % formatting; storing the raw template would show the
    operator `Timeout from %s`."""
    logging.error('Timeout from %s', '10.0.0.9')

    assert state_with_log.recent_incidents()[0].message == 'Timeout from 10.0.0.9'


def test_an_exception_traceback_does_not_reach_the_page(state_with_log):
    """The operator needs the sentence, not the stack — and a traceback in a
    table cell makes the panel unreadable for the failures around it."""
    try:
        raise RuntimeError('broker exploded')
    except RuntimeError:
        logging.exception('Publishing the burst from 10.0.0.7 failed')

    incident = state_with_log.recent_incidents()[0]

    assert 'Traceback' not in incident.message
    assert '10.0.0.7' in incident.message


def test_the_log_is_bounded_so_a_broken_night_cannot_exhaust_memory(state_with_log):
    """A sensor in a reconnect loop can log for hours with nobody watching."""
    for i in range(state_with_log.incidents.maxlen + 50):
        logging.warning(f'ocorrencia {i}')

    assert len(state_with_log.incidents) == state_with_log.incidents.maxlen
    assert state_with_log.recent_incidents()[0].message.endswith(
        str(state_with_log.incidents.maxlen + 49))


def test_installing_twice_does_not_duplicate_every_incident(state_with_log):
    """Two handlers on the root logger would show the operator each failure
    twice and halve the useful depth of the ring."""
    second = install_incident_log(state_with_log)

    logging.error('uma vez so')

    assert len(state_with_log.recent_incidents()) == 1
    uninstall_incident_log(second)


def test_the_levels_shown_are_the_ones_the_page_knows_how_to_paint():
    assert INCIDENT_LEVELS == ('WARNING', 'ERROR', 'CRITICAL')


# --------------------------------------------------------------------------
# What the operator sees
# --------------------------------------------------------------------------

def test_the_page_lists_the_failures(client):
    c, _ = client
    logging.error('Config not delivered to 10.0.0.7: [Errno 32] Broken pipe')

    body = c.get('/').data.decode()

    assert '10.0.0.7' in body
    assert 'Broken pipe' in body


def test_a_healthy_server_says_so_instead_of_showing_an_empty_table(client):
    """An empty area reads as "the panel is broken"; the sentence reads as "there
    is nothing wrong", which is the fact the operator came for."""
    c, _ = client

    body = c.get('/').data.decode()

    assert 'Nenhuma ocorrencia' in body


def test_a_failure_to_save_the_configuration_is_recorded_where_it_is_seen(client, monkeypatch):
    """The 503 tells whoever clicked. The incident is what the next person to
    open the page — or the same person after a reload — has to go on."""
    import sqlite3

    from store import config_store

    def refuse(*args, **kwargs):
        raise sqlite3.OperationalError('attempt to write a readonly database')

    c, state = client
    monkeypatch.setattr(state, 'db_path', '/tmp/does-not-matter.db')
    monkeypatch.setattr(config_store, 'write_config', refuse)

    response = c.post('/sampling', data={'sampling_hz': '200'})

    assert response.status_code == 503
    assert 'readonly' in state.recent_incidents()[0].message
