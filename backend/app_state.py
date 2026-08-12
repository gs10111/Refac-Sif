import threading
import datetime
import dataclasses
from collections import deque
from dataclasses import dataclass

from config.settings import (
    DEFAULT_SLEEP_MIN, DEFAULT_IDLE_MIN,
    DEFAULT_MAX_ACQ, DEFAULT_COOLDOWN_SEC, DEFAULT_UPDATE,
    HISTORY_MAX_CONNECTIONS, INCIDENTS_MAX, SAMPLING_CODE,
)
from store import config_store


@dataclass
class DeviceConfig:
    sleep_min:    int = DEFAULT_SLEEP_MIN
    idle_min:     int = DEFAULT_IDLE_MIN
    max_acq:      int = DEFAULT_MAX_ACQ
    cooldown_sec: int = DEFAULT_COOLDOWN_SEC
    # The ODR nibble every device is told to run. It is not editable from the
    # web form: the rate is a fleet-wide decision taken at deploy time, and a
    # form field would let one careless save re-rate every sensor in the plant.
    sampling_code: int = SAMPLING_CODE


# The levels the page knows how to paint. INFO is the server saying it is
# working: mixed in, it would bury the three lines that matter under a hundred
# that do not.
INCIDENT_LEVELS = ('WARNING', 'ERROR', 'CRITICAL')


@dataclass
class IncidentEntry:
    """One failure, as the operator needs to read it."""
    timestamp: datetime.datetime
    level:     str
    message:   str


@dataclass
class ConnectionEntry:
    ip:         str
    timestamp:  datetime.datetime
    n_samples:  int
    battery_mv: int
    ota_sent:   bool = False   # this device received update=1
    # From the 7-byte trailer. None means a device still on firmware that ends
    # its message with two bytes of battery — not a device that failed to say.
    firmware:     str | None = None
    effective_hz: int | None = None
    complete:   bool = True    # the promised payload arrived; False means it did not
                               # and the device has already discarded it


class AppState:
    """Shared in-memory state between Flask web server and TCP server threads.
    All mutations to config, connections and the OTA arming must be protected
    by lock — use the methods below rather than touching the fields directly.
    """

    def __init__(self, db_path: str | None = None) -> None:
        # With a db_path the file is the ONLY source of the configuration: it is
        # read on every access, so a save on the page reaches the next device to
        # connect without a restart, and survives one. Without a db_path the
        # configuration lives in memory and goes back to the defaults when the
        # process ends — the behaviour this server always had, kept for tests and
        # for anyone running without a writable path.
        #
        # The connection history and the OTA arming stay in memory either way.
        # Only the configuration was moved to disk; promising more would tell an
        # operator the history survives a restart, and it does not.
        self.db_path:     str | None             = db_path
        self._config:     DeviceConfig           = DeviceConfig()
        self.connections: deque[ConnectionEntry] = deque(maxlen=HISTORY_MAX_CONNECTIONS)
        # Failures, newest last. In memory like the history: a restart clears it,
        # and the README says so rather than implying an audit trail.
        self.incidents:   deque[IncidentEntry]   = deque(maxlen=INCIDENTS_MAX)
        self.ota_armed:   bool                   = bool(DEFAULT_UPDATE)
        # Reentrant: the incident log records from whichever thread hit the
        # failure, and a failure logged from inside a section that already holds
        # this lock would deadlock the server on a plain Lock. No path does that
        # today; the point is that adding one must not be a way to hang the
        # acquisition.
        self.lock:        threading.RLock        = threading.RLock()

    @property
    def config(self) -> DeviceConfig:
        """The configuration in force.

        File-backed: a fresh snapshot, read now. In memory: the live object, so
        the field-by-field mutation the page used to do still works.
        """
        if self.db_path is None:
            return self._config
        return DeviceConfig(**config_store.read_config(self.db_path))

    def update_config(self, **fields) -> None:
        """Write the fields given and leave every other one untouched.

        Partial on purpose: the rate has its own route and the duty cycle its own
        form, and neither may drag the other along. Validation lives in the store,
        so a rejected value writes nothing in either mode.
        """
        if self.db_path is not None:
            config_store.write_config(self.db_path, **fields)
            return
        for field, value in fields.items():
            config_store.validate_field(field, value)
        with self.lock:
            for field, value in fields.items():
                setattr(self._config, field, value)

    def record_incident(self, level: str, message: str,
                        when: datetime.datetime | None = None) -> None:
        """Add a failure to the log the page shows.

        Called from the logging handler, which runs on whichever thread hit the
        failure — hence the lock.
        """
        entry = IncidentEntry(
            timestamp=when or datetime.datetime.now(),
            level=level,
            message=message,
        )
        with self.lock:
            self.incidents.append(entry)

    def recent_incidents(self, limit: int | None = None) -> list[IncidentEntry]:
        """The most recent failures, newest first.

        Newest first because the operator opens the page to find out what is
        wrong NOW; the oldest line of a long night is the least useful one.
        """
        with self.lock:
            newest_first = list(reversed(self.incidents))
        return newest_first if limit is None else newest_first[:limit]

    def set_ota_armed(self, armed: bool) -> None:
        """Arm or disarm the one-shot OTA flag (POST /ota)."""
        with self.lock:
            self.ota_armed = bool(armed)

    def take_config_for_send(self) -> tuple[DeviceConfig, bool]:
        """Snapshot the config and claim the one-shot OTA flag.

        Returns (config copy, ota). Reading the config and clearing the arming
        happen under a single lock acquisition, so with several devices
        transmitting at once exactly one of them gets ota=True.
        """
        with self.lock:
            ota = self.ota_armed
            self.ota_armed = False
            return dataclasses.replace(self.config), ota

    def rearm_ota(self) -> None:
        """Give the arming back after a failed send — nobody received the flag."""
        with self.lock:
            self.ota_armed = True
