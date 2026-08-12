"""The fleet configuration survives a restart of the server.

Acceptance criterion: what the operator saves on the page is what the next
device to connect receives — including after the server process is restarted,
and without editing a systemd unit or reaching for an environment variable.

All SQL lives in store/config_store.py: no Flask, no socket, so every test here
runs against a temporary database file and nothing else.
"""
import sqlite3

import pytest

from protocol.packet import SAMPLING_CODES
from store.config_store import (
    CONFIG_FIELDS, DEFAULTS, init_db, read_config, write_config,
)


@pytest.fixture
def db(tmp_path):
    path = str(tmp_path / 'sif.db')
    init_db(path)
    return path


def test_a_fresh_database_holds_the_documented_defaults(db):
    """A server started against an empty disk must behave exactly like the one
    that had no database at all — same numbers, same rate."""
    config = read_config(db)

    assert config == DEFAULTS


def test_initialising_twice_does_not_undo_what_the_operator_saved(db):
    """Every boot calls init_db. A seed that overwrote would silently return the
    fleet to defaults on each restart, which is the opposite of persisting."""
    write_config(db, sleep_min=99)

    init_db(db)

    assert read_config(db)['sleep_min'] == 99


def test_what_was_written_is_what_is_read_back_after_reopening(db):
    """The point of the file: a different connection, later, sees it."""
    write_config(db, sleep_min=15, idle_min=7, sampling_code=SAMPLING_CODES['200'])

    reread = read_config(db)

    assert reread['sleep_min'] == 15
    assert reread['idle_min'] == 7
    assert reread['sampling_code'] == SAMPLING_CODES['200']


def test_writing_one_field_leaves_the_others_alone(db):
    """The rate is saved by its own route, the duty cycle by the form. Neither
    may drag the other along."""
    write_config(db, sampling_code=SAMPLING_CODES['25'])

    config = read_config(db)

    assert config['sampling_code'] == SAMPLING_CODES['25']
    assert config['sleep_min'] == DEFAULTS['sleep_min']
    assert config['cooldown_sec'] == DEFAULTS['cooldown_sec']


def test_a_field_that_is_not_part_of_the_config_is_refused(db):
    """The field name reaches SQL. Refusing anything unknown by name is what
    keeps a request parameter from becoming a column reference."""
    with pytest.raises(ValueError):
        write_config(db, drop_table=1)

    assert read_config(db) == DEFAULTS


def test_a_value_that_does_not_fit_the_protocol_is_refused(db):
    """Every field is a uint16 on the wire; 0 stops the device dead."""
    for value in (0, -1, 65536):
        with pytest.raises(ValueError):
            write_config(db, sleep_min=value)

    assert read_config(db)['sleep_min'] == DEFAULTS['sleep_min']


def test_a_rate_the_part_cannot_run_is_refused(db):
    """Stored, it would be sent to every device on the next connection."""
    with pytest.raises(ValueError):
        write_config(db, sampling_code=13)

    assert read_config(db)['sampling_code'] == DEFAULTS['sampling_code']


def test_writing_nothing_is_not_an_accident_that_wipes_the_row(db):
    write_config(db, sleep_min=15)

    write_config(db)

    assert read_config(db)['sleep_min'] == 15


def test_the_database_runs_in_wal_so_a_reader_never_blocks_a_capture(db):
    """The TCP threads read this file while the page writes it. Without WAL the
    writer takes an exclusive lock and a device connecting at that moment waits
    on it — with the socket timeout running."""
    with sqlite3.connect(db) as conn:
        mode = conn.execute('PRAGMA journal_mode').fetchone()[0]

    assert mode.lower() == 'wal'


def test_a_row_missing_a_column_reads_as_the_default(tmp_path):
    """Defensive read: a database written by an older build has fewer columns.
    Failing to start there would strand the operator with a file they cannot
    delete without losing the rest of the configuration."""
    path = str(tmp_path / 'old.db')
    with sqlite3.connect(path) as conn:
        conn.execute('CREATE TABLE config (id INTEGER PRIMARY KEY CHECK (id = 1), sleep_min INTEGER)')
        conn.execute('INSERT INTO config (id, sleep_min) VALUES (1, 33)')

    init_db(path)
    config = read_config(path)

    assert config['sleep_min'] == 33
    assert config['sampling_code'] == DEFAULTS['sampling_code']


def test_a_path_that_cannot_be_opened_fails_loudly(tmp_path):
    """Fail-closed at boot: falling back to memory would mean the configuration
    the operator saved disappears at the next restart, with nobody told."""
    import sqlite3

    with pytest.raises(sqlite3.OperationalError):
        init_db(str(tmp_path / 'nao' / 'existe' / 'sif.db'))


def test_every_config_field_has_a_default_and_nothing_else_does():
    """The two lists are the contract between the store and the page."""
    assert set(CONFIG_FIELDS) == set(DEFAULTS)
