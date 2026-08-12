import threading
import datetime
import dataclasses
from collections import deque
from dataclasses import dataclass

from config.settings import (
    DEFAULT_SLEEP_MIN, DEFAULT_IDLE_MIN,
    DEFAULT_MAX_ACQ, DEFAULT_COOLDOWN_SEC, DEFAULT_UPDATE,
    HISTORY_MAX_CONNECTIONS, SAMPLING_CODE,
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


@dataclass
class ConnectionEntry:
    ip:         str
    timestamp:  datetime.datetime
    n_samples:  int
    battery_mv: int
    ota_sent:   bool = False   # this device received update=1
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
        self.ota_armed:   bool                   = bool(DEFAULT_UPDATE)
        self.lock:        threading.Lock         = threading.Lock()

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
