import struct

SAMPLE_SIZE_BYTES  = 18
HEADER_SIZE_BYTES  =  4
BATTERY_SIZE_BYTES =  2
SERVER_CONFIG_SIZE =  8  # 4 × uint16_t

# Column order matches the 18-byte sample layout
SAMPLE_COLUMNS = ['timestamp', 'accel_x', 'gyro_x', 'accel_y', 'gyro_y', 'accel_z', 'gyro_z', 'temp']

def parse_sample(raw: bytes) -> list:
    timestamp = int.from_bytes(raw[:4], byteorder='little')
    fields    = list(struct.unpack_from('<7h', raw, 4))
    return [timestamp] + fields

def pack_server_config(sleep_min, idle_min, max_acq, cooldown_sec) -> bytes:
    return struct.pack('<HHHH', sleep_min, idle_min, max_acq, cooldown_sec)
