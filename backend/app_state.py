import threading
import datetime
import dataclasses
from collections import deque
from dataclasses import dataclass

from config.settings import (
    DEFAULT_SLEEP_MIN, DEFAULT_IDLE_MIN,
    DEFAULT_MAX_ACQ, DEFAULT_COOLDOWN_SEC, DEFAULT_UPDATE,
)


@dataclass
class DeviceConfig:
    sleep_min:    int = DEFAULT_SLEEP_MIN
    idle_min:     int = DEFAULT_IDLE_MIN
    max_acq:      int = DEFAULT_MAX_ACQ
    cooldown_sec: int = DEFAULT_COOLDOWN_SEC


@dataclass
class ConnectionEntry:
    ip:         str
    timestamp:  datetime.datetime
    n_samples:  int
    battery_mv: int
    ota_sent:   bool = False   # this device received update=1


class AppState:
    """Shared in-memory state between Flask web server and TCP server threads.
    All mutations to config, connections and the OTA arming must be protected
    by lock — use the methods below rather than touching the fields directly.
    """

    def __init__(self) -> None:
        self.config:      DeviceConfig          = DeviceConfig()
        self.connections: deque[ConnectionEntry] = deque(maxlen=50)
        self.ota_armed:   bool                   = bool(DEFAULT_UPDATE)
        self.lock:        threading.Lock         = threading.Lock()

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
