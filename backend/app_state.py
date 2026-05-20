import threading
import datetime
from collections import deque
from dataclasses import dataclass

from config.settings import (
    DEFAULT_SLEEP_MIN, DEFAULT_IDLE_MIN,
    DEFAULT_MAX_ACQ, DEFAULT_COOLDOWN_SEC,
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


class AppState:
    def __init__(self):
        self.config:      DeviceConfig          = DeviceConfig()
        self.connections: deque[ConnectionEntry] = deque(maxlen=50)
        self.lock:        threading.Lock         = threading.Lock()
