import struct

SAMPLE_SIZE_BYTES  = 18
HEADER_SIZE_BYTES  =  4
BATTERY_SIZE_BYTES =  2
SERVER_CONFIG_SIZE = 10  # 5 × uint16_t

# Column order matches the 18-byte sample layout. Names are the ones the
# production servers write (pyFiles/win_server.py:45) — the historical CSV
# corpus and tools/analysis/cliente_local_csv.py depend on them.
SAMPLE_COLUMNS = ['timestamp', 'x_data', 'x_gyro', 'y_data', 'y_gyro', 'z_data', 'z_gyro', 'temp']
BATTERY_COLUMN = 'battery_voltage'
CSV_COLUMNS    = SAMPLE_COLUMNS + [BATTERY_COLUMN]

def parse_sample(raw: bytes) -> list:
    timestamp = int.from_bytes(raw[:4], byteorder='little')
    fields    = list(struct.unpack_from('<7h', raw, 4))
    return [timestamp] + fields

def pack_server_config(sleep_min, idle_min, max_acq, cooldown_sec, update) -> bytes:
    """Pack the 10-byte server → ESP32 response.

    Field order is the production one (pyFiles/win_server.py:114):
    sleep_min, idle_min, max_acq, cooldown_sec, update.

    `update` has no default on purpose: arming a device into OTA is a
    deliberate act, so every call site must say which one it means.
    """
    if update not in (0, 1):
        raise ValueError(f'update must be 0 or 1, got {update!r}')
    return struct.pack('<HHHHH', sleep_min, idle_min, max_acq, cooldown_sec, update)
