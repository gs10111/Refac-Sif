import os

SERVER_IP   = os.getenv('SERVER_IP',   '0.0.0.0')
SERVER_PORT = int(os.getenv('SERVER_PORT', '12345'))
BUFFER_SIZE = 990

GDRIVE_PATH = os.getenv(
    'GDRIVE_PATH',
    '/run/user/1000/gvfs/google-drive:host=gmail.com,user=valev6852/0ALPQP0Ju3mfQUk9PVA'
)

CLIENT_TIMEOUT_SEC  = 6.0
RESPONSE_TIMEOUT_SEC = 5.0

DEFAULT_SLEEP_MIN      = 240
DEFAULT_IDLE_MIN       =  20
DEFAULT_MAX_ACQ        =   5
DEFAULT_COOLDOWN_SEC   =   5
