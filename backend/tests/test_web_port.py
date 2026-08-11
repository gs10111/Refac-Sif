"""The Flask UI port comes from the environment.

Acceptance criterion: the operator moves the web UI to another port without
editing code. WEB_PORT sets it; an absent WEB_PORT keeps the 8080 the plant
already has in its bookmarks; a value that is not a port stops the boot instead
of silently falling back to a port nobody asked for.

No network here. web_server_main runs with Flask's run() captured, so the test
proves which port *would* be bound without binding one — the same reason the
socket is faked in test_tcp_server.py.
"""
import importlib
import os

import pytest

from app_state import AppState
import config.settings
from web import server as web_server


@pytest.fixture
def reload_settings():
    """Re-run config.settings so os.getenv is read again.

    Teardown clears WEB_PORT and reloads unconditionally: the rest of the suite
    imports settings once at collection, and a module left holding a test's
    value would leak into whatever runs next.
    """
    yield lambda: importlib.reload(config.settings)
    os.environ.pop('WEB_PORT', None)
    importlib.reload(config.settings)


@pytest.fixture
def captured_run(monkeypatch):
    """Capture the kwargs of Flask.run instead of serving."""
    calls = []
    monkeypatch.setattr('flask.Flask.run', lambda self, **kwargs: calls.append(kwargs))
    return calls


def test_web_server_main_binds_the_configured_port(monkeypatch, captured_run):
    monkeypatch.setattr(config.settings, 'WEB_PORT', 9091)

    web_server.web_server_main(AppState())

    assert captured_run[0]['port'] == 9091


def test_an_explicit_port_still_wins_over_the_configured_one(monkeypatch, captured_run):
    """The caller's argument is the override; the setting is only the default."""
    monkeypatch.setattr(config.settings, 'WEB_PORT', 9091)

    web_server.web_server_main(AppState(), port=9092)

    assert captured_run[0]['port'] == 9092


def test_web_port_comes_from_the_environment(monkeypatch, reload_settings):
    monkeypatch.setenv('WEB_PORT', '9000')

    assert reload_settings().WEB_PORT == 9000


def test_web_port_defaults_to_8080_when_unset(monkeypatch, reload_settings):
    monkeypatch.delenv('WEB_PORT', raising=False)

    assert reload_settings().WEB_PORT == 8080


def test_an_unusable_web_port_stops_the_boot(monkeypatch, reload_settings):
    """Fail-closed: a typo is not quietly replaced by 8080, which would leave the
    operator pointing a browser at a port the server never meant to serve."""
    monkeypatch.setenv('WEB_PORT', 'oitenta')

    with pytest.raises(ValueError):
        reload_settings()
