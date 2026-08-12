import os

from protocol.packet import DEFAULT_SAMPLING_HZ, sampling_code_from_hz

SERVER_IP   = os.getenv('SERVER_IP',   '0.0.0.0')
SERVER_PORT = int(os.getenv('SERVER_PORT', '12345'))

# The web UI port. Absent means 8080 — the port the plant already has in its
# bookmarks. A value that is not an integer raises here, at import, rather than
# falling back to 8080: a server answering on a port the operator did not
# choose is worse than one that refuses to start.
WEB_PORT    = int(os.getenv('WEB_PORT',    '8080'))

GDRIVE_PATH = os.getenv(
    'GDRIVE_PATH',
    '/run/user/1000/gvfs/google-drive:host=gmail.com,user=valev6852/0ALPQP0Ju3mfQUk9PVA'
)

CLIENT_TIMEOUT_SEC = 6.0

# Twice the 700000-byte device ring buffer: sanity headroom against a corrupt
# header, not a tuned limit and not part of the contract. The real maximum on
# the wire is 38888 × 18 = 699984 bytes.
MAX_PAYLOAD_BYTES = 1400000

# Written to the CSV and to the connection history when the battery reading
# never arrived — same sentinel the production server uses.
BATTERY_INVALID = -1

# Connection history depth. Under D1 a wake is up to max_acq acquisitions, each
# its own connection and its own row: five sensors is ~25 rows per round, so
# this holds about twenty rounds. What matters is not the number but how many
# rounds the operator can still see the OTA row for.
HISTORY_MAX_CONNECTIONS = 500

# Acquisition rate sent to every device, as the ODR nibble the part expects.
# An empty variable reads as "not set" — `Environment="SIF_SAMPLING_HZ="` in a
# systemd unit is a typo, not a request for a blank rate. Anything else that is
# not a supported rate raises here, at import: see the comment on WEB_PORT.
SAMPLING_HZ   = os.getenv('SIF_SAMPLING_HZ', '').strip() or DEFAULT_SAMPLING_HZ
SAMPLING_CODE = sampling_code_from_hz(SAMPLING_HZ)

# Where the fleet configuration lives. It is written by the page and read by the
# TCP threads once per connection, so a change reaches the next device that
# transmits instead of waiting for a restart.
#
# Always on: a path that cannot be opened raises at boot rather than silently
# falling back to memory, because falling back would mean the operator's saved
# configuration disappears at the next restart without anyone being told.
DB_PATH = os.getenv('SIF_DB_PATH', '').strip() or 'sif.db'

# --- MQTT / ThingsBoard -----------------------------------------------------
# Publishing is off until asked for: a server that has never been pointed at a
# broker must not try to reach one. Once asked for, an incomplete configuration
# raises here, at import — a server that looks configured for ThingsBoard and
# quietly publishes nowhere is worse than one that refuses to start.
MQTT_ENABLED = os.getenv('SIF_MQTT_ENABLED', '').strip().lower() in ('1', 'true', 'yes')
MQTT_HOST    = os.getenv('SIF_MQTT_HOST', '').strip()
MQTT_PORT    = int(os.getenv('SIF_MQTT_PORT', '1883'))
# Topic prefix and block size are operational knobs, not credentials: the
# gateway subscribes to `<prefix>/telemetry/#` and `<prefix>/burst/#`.
MQTT_TOPIC_PREFIX = os.getenv('SIF_MQTT_TOPIC_PREFIX', 'sif').strip() or 'sif'
MQTT_CHUNK_SIZE   = int(os.getenv('SIF_MQTT_CHUNK_SIZE', '100'))

if MQTT_ENABLED and not MQTT_HOST:
    raise ValueError(
        'SIF_MQTT_ENABLED esta ligado mas SIF_MQTT_HOST esta vazio: '
        'sem broker nao ha para onde publicar.'
    )

# Failures kept for the page. Two hundred covers a bad night of one sensor in a
# reconnect loop without letting a broken device exhaust memory — the log is
# bounded for the same reason the connection history is.
INCIDENTS_MAX = 200

# How many of them the page shows. The rest stay in the ring: a panel that
# scrolls past twenty lines stops being read at all.
INCIDENTS_SHOWN = 20

DEFAULT_SLEEP_MIN      = 240
DEFAULT_IDLE_MIN       =  20
DEFAULT_MAX_ACQ        =   5
DEFAULT_COOLDOWN_SEC   =   5
DEFAULT_UPDATE         =   0  # OTA disarmed at startup
