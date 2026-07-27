"""One end-to-end exchange over a real loopback socket.

Every other handle_client test drives a MagicMock, which returns whatever it was
queued REGARDLESS OF THE BYTE COUNT IT WAS ASKED FOR. That is why a recv_exact
asking for the wrong number of bytes would pass the whole suite (docs/backend,
L6). A real socket cannot ignore a byte count, so this test is the one place the
framing is exercised against the operating system rather than against a mock.

Deterministic by construction, not by tuning: the client connects and sends
before the server accepts, the payload is 60 bytes so it fits in the socket
buffer without a second write, handle_client runs in the test thread, and the
response is read after it returns because the bytes are already in the client's
receive queue. No server_main, no accept loop, no threads, no sleeps, no polling.
"""

import socket
import struct

import pytest

from app_state import AppState
from protocol.packet import (
    HEADER_SIZE_BYTES, SAMPLE_SIZE_BYTES, BATTERY_SIZE_BYTES,
    SERVER_CONFIG_SIZE,
)
from server import tcp_server
from server.tcp_server import handle_client


BATTERY_MV = 3800
N_SAMPLES  = 3

# Failsafe only. A 60-byte loopback exchange takes microseconds; this exists so
# a regression FAILS instead of hanging CI, not as a value anyone should tune.
GUARD_TIMEOUT_SEC = 5.0


@pytest.fixture
def socket_pair():
    """A connected (client, accepted server) pair on a kernel-assigned port."""
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.bind(('127.0.0.1', 0))          # port 0 — the kernel picks a free one
    listener.listen(1)
    listener.settimeout(GUARD_TIMEOUT_SEC)

    client = socket.create_connection(listener.getsockname(),
                                      timeout=GUARD_TIMEOUT_SEC)
    try:
        yield client, listener
    finally:
        client.close()
        listener.close()


def test_full_exchange_over_a_real_socket(socket_pair):
    client, listener = socket_pair
    while not tcp_server.data_queue.empty():
        tcp_server.data_queue.get_nowait()

    state = AppState()
    state.config.sleep_min = 111
    state.set_ota_armed(True)

    body   = bytes(SAMPLE_SIZE_BYTES * N_SAMPLES)
    header = len(body).to_bytes(HEADER_SIZE_BYTES, 'little')
    batt   = BATTERY_MV.to_bytes(BATTERY_SIZE_BYTES, 'little')
    client.sendall(header + body + batt)      # 60 bytes, one write

    conn, addr = listener.accept()
    handle_client(conn, addr, state)

    # The response was written before handle_client closed its end, so it is
    # already sitting in this socket's receive queue.
    response = client.recv(SERVER_CONFIG_SIZE)
    sleep_min, idle_min, max_acq, cooldown_sec, update = struct.unpack(
        '<HHHHH', response)

    assert len(response) == SERVER_CONFIG_SIZE
    assert sleep_min     == 111
    assert update        == 1

    entry = state.connections[0]
    assert entry.ip         == '127.0.0.1'
    assert entry.n_samples  == N_SAMPLES
    assert entry.battery_mv == BATTERY_MV
    assert entry.ota_sent   is True

    ip, _timestamp, samples = tcp_server.data_queue.get_nowait()
    assert ip          == '127.0.0.1'
    assert len(samples) == N_SAMPLES
    assert samples[0][-1] == BATTERY_MV
