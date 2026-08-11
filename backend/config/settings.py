import os

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

DEFAULT_SLEEP_MIN      = 240
DEFAULT_IDLE_MIN       =  20
DEFAULT_MAX_ACQ        =   5
DEFAULT_COOLDOWN_SEC   =   5
DEFAULT_UPDATE         =   0  # OTA disarmed at startup
