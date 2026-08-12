"""The fleet configuration, on disk.

Every statement of SQL in the project lives here: no Flask, no socket, so the
whole module is testable against a temporary file. The web page writes it, the
TCP threads read it once per connection — which is what lets a change take
effect on the next device that transmits instead of on the next restart.

One row, always id = 1. There is one fleet, and a table that could hold two
configurations would need a rule about which one wins.
"""
import sqlite3

from protocol.packet import UINT16_MAX, is_valid_sampling_code
from config.settings import (
    DEFAULT_SLEEP_MIN, DEFAULT_IDLE_MIN, DEFAULT_MAX_ACQ, DEFAULT_COOLDOWN_SEC,
    SAMPLING_CODE,
)

# The columns the page may write. The field name reaches SQL, so anything not on
# this list is refused by name before a statement is built.
CONFIG_FIELDS = ('sleep_min', 'idle_min', 'max_acq', 'cooldown_sec', 'sampling_code')

# What a fresh database holds — the same numbers a server with no database at
# all used to send, so installing this feature changes no device's behaviour.
DEFAULTS = {
    'sleep_min':     DEFAULT_SLEEP_MIN,
    'idle_min':      DEFAULT_IDLE_MIN,
    'max_acq':       DEFAULT_MAX_ACQ,
    'cooldown_sec':  DEFAULT_COOLDOWN_SEC,
    'sampling_code': SAMPLING_CODE,
}


def _connect(path):
    conn = sqlite3.connect(path, timeout=5.0)
    # WAL because the TCP threads read this file while the page writes it. In the
    # default rollback journal a writer takes an exclusive lock, and a device
    # connecting at that moment waits on it with its socket timeout running.
    conn.execute('PRAGMA journal_mode=WAL')
    conn.row_factory = sqlite3.Row
    return conn


def init_db(path):
    """Create the table and the single row if they are not there yet.

    Called on every boot, so it must never overwrite: a seed that wrote the
    defaults each time would return the fleet to them on every restart, which is
    the opposite of persisting. Columns added by a later build are added here
    too, which is what lets a database written by an older one keep working.
    """
    with _connect(path) as conn:
        conn.execute(
            'CREATE TABLE IF NOT EXISTS config ('
            '  id INTEGER PRIMARY KEY CHECK (id = 1)'
            ')'
        )
        existing = {row['name'] for row in conn.execute('PRAGMA table_info(config)')}
        for field in CONFIG_FIELDS:
            if field not in existing:
                conn.execute(f'ALTER TABLE config ADD COLUMN {field} INTEGER')
        # One statement, not check-then-insert: two processes booting together
        # would both see no row and both insert.
        conn.execute('INSERT OR IGNORE INTO config (id) VALUES (1)')


def read_config(path):
    """The configuration in force, as a plain dict.

    A column that is absent or NULL reads as its default rather than raising: a
    database written by an older build must not strand the operator with a file
    they can only fix by deleting it, and losing the rest of the configuration.
    """
    with _connect(path) as conn:
        row = conn.execute('SELECT * FROM config WHERE id = 1').fetchone()

    if row is None:
        return dict(DEFAULTS)

    columns = row.keys()
    return {
        field: (row[field] if field in columns and row[field] is not None
                else DEFAULTS[field])
        for field in CONFIG_FIELDS
    }


def validate_field(field, value):
    if field not in CONFIG_FIELDS:
        raise ValueError(f'Campo de configuracao desconhecido: {field!r}.')

    if field == 'sampling_code':
        if not is_valid_sampling_code(value):
            raise ValueError(
                f'Codigo de amostragem invalido: {value!r}. '
                f'Guardado, ele iria para todo device na proxima conexao.'
            )
        return value

    # Every other field is a uint16 on the wire, and 0 stops the device dead.
    if not isinstance(value, int) or isinstance(value, bool):
        raise ValueError(f'{field}: {value!r} nao e um inteiro.')
    if value <= 0 or value > UINT16_MAX:
        raise ValueError(
            f'{field}: {value} fora da faixa do protocolo (1 a {UINT16_MAX}).'
        )
    return value


def write_config(path, **fields):
    """Write the fields given, leaving every other one untouched.

    Partial by design: the rate is saved by its own route and the duty cycle by
    the form, and neither may drag the other along. Validation happens before any
    statement is built, so a rejected value writes nothing at all.
    """
    validated = {field: validate_field(field, value) for field, value in fields.items()}
    if not validated:
        return

    assignments = ', '.join(f'{field} = ?' for field in validated)
    with _connect(path) as conn:
        conn.execute(f'UPDATE config SET {assignments} WHERE id = 1',
                     tuple(validated.values()))
