import struct

SAMPLE_SIZE_BYTES  = 18
HEADER_SIZE_BYTES  =  4
BATTERY_SIZE_BYTES =  2
SERVER_CONFIG_SIZE = 12  # 6 × uint16_t

# ODR nibble of ACCEL_CONFIG0 / GYRO_CONFIG0 on the ICM-42688-P, keyed by the
# rate in Hz as the operator writes it.
#
# Only the rates where accelerometer AND gyroscope both run: codes 12-14 are
# Reserved for the gyroscope on this part — they are not an accelerometer-only
# low-power mode to be discovered later. Code 15 (500 Hz) works on both and is
# left out by scope, not by hardware.
SAMPLING_CODES = {
    '200':  7,
    '100':  8,
    '50':   9,
    '25':  10,
    '12.5': 11,
}

# Inverse table. A dict rather than a linear search makes the codes' uniqueness
# structural instead of implied.
_HZ_BY_SAMPLING_CODE = {code: hz for hz, code in SAMPLING_CODES.items()}

# A server with no opinion about the rate sends this. 0 is Reserved as an ODR
# nibble, so a firmware that whitelists codes keeps whatever rate it is running.
SAMPLING_CODE_NO_CHANGE = 0

DEFAULT_SAMPLING_HZ = '50'

# Every field of the response travels as a uint16. This is the width of the
# wire field, not a tunable policy.
UINT16_MAX = 65535

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

def sampling_code_from_hz(hz) -> int:
    """Translate a rate in Hz into the ODR nibble that travels on the wire.

    Accepts what an operator would type: '50', '50.0', 50, 12.5. Raises rather
    than falling back, because a rate silently replaced by another is a fleet
    sampling at something nobody chose and nobody can see.
    """
    key = str(hz).strip()
    if key.endswith('.0'):          # '50.0' and '50' are the same intent
        key = key[:-2]
    if key not in SAMPLING_CODES:
        accepted = ', '.join(f'{rate} Hz' for rate in SAMPLING_CODES)
        raise ValueError(
            f'Taxa de amostragem invalida: {hz!r}. Aceitas: {accepted}.'
        )
    return SAMPLING_CODES[key]


def is_valid_sampling_code(code) -> bool:
    """Whether a code is one of the five the part can actually run.

    The mirror of is_valid_sampling_code() in lib/protocol/packet.h: what the
    firmware refuses to apply, the server refuses to store or send.
    """
    return code in _HZ_BY_SAMPLING_CODE


def sampling_hz_from_code(code: int) -> str:
    """The rate a code stands for, for logs and for the connection history."""
    if code == SAMPLING_CODE_NO_CHANGE:
        return 'inalterada'
    return _HZ_BY_SAMPLING_CODE.get(code, 'desconhecida')


def pack_server_config(sleep_min, idle_min, max_acq, cooldown_sec, update,
                       sampling_code) -> bytes:
    """Pack the 12-byte server → ESP32 response.

    Field order is the wire order, and it extends the production one
    (pyFiles/win_server.py:114) at the end: sleep_min, idle_min, max_acq,
    cooldown_sec, update, sampling_code. The five original fields keep their
    offsets, so a device reading the old layout still finds them where it
    expects — what it will not find is the twelfth byte, and a short frame
    leaves its config untouched by design.

    Neither `update` nor `sampling_code` has a default: arming a device into
    OTA and changing the rate of a whole fleet are both deliberate acts, so
    every call site says which one it means.
    """
    if update not in (0, 1):
        raise ValueError(f'update must be 0 or 1, got {update!r}')
    if (sampling_code != SAMPLING_CODE_NO_CHANGE
            and sampling_code not in _HZ_BY_SAMPLING_CODE):
        accepted = ', '.join(
            f'{code} ({_HZ_BY_SAMPLING_CODE[code]} Hz)'
            for code in sorted(_HZ_BY_SAMPLING_CODE)
        )
        raise ValueError(
            f'Codigo de amostragem invalido: {sampling_code!r}. '
            f'Aceitos: {accepted} ou {SAMPLING_CODE_NO_CHANGE} (sem mudanca).'
        )
    return struct.pack('<HHHHHH', sleep_min, idle_min, max_acq, cooldown_sec,
                       update, sampling_code)
