"""
TCP server that receives IMU data from ESP32 devices, saves it to CSV,
and sends back a ServerConfig packet with the current operating parameters.

Protocol (per connection):
  ESP32 → server:
    [4 bytes] uint32 little-endian  — total bytes of sample data to follow
    [N bytes] sample frames         — each frame is SAMPLE_SIZE_BYTES (18 bytes)
    [2 bytes] uint16 little-endian  — battery voltage in mV

  server → ESP32:
    [10 bytes] ServerConfig         — 5 × uint16 little-endian
                                      (sleep_min, idle_min, max_acq,
                                       cooldown_sec, update)

Threads (started from __main__):
  server_main      — accepts TCP connections, dispatches handle_client via thread pool
  web_server_main  — Flask UI on port 8080 (config editing + connection history)
  save_data        — consumes data_queue, writes CSV, copies to Google Drive
  exit_monitor     — waits for 'q' on stdin to stop the server
"""

import socket
import csv
import queue
import threading
import logging
import datetime
import os
import subprocess
from concurrent.futures import ThreadPoolExecutor

from protocol.packet import (
    SAMPLE_SIZE_BYTES, HEADER_SIZE_BYTES, BATTERY_SIZE_BYTES, CSV_COLUMNS,
    parse_sample, pack_server_config
)
from config.settings import (
    SERVER_IP, SERVER_PORT, GDRIVE_PATH,
    CLIENT_TIMEOUT_SEC, MAX_PAYLOAD_BYTES, BATTERY_INVALID,
)
from app_state import AppState, ConnectionEntry

logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')

running    = True       # set to False by exit_monitor to stop all loops
data_queue = queue.Queue()  # (ip, timestamp, samples) tuples consumed by save_data


def save_data():
    """Worker thread: writes received samples to CSV and copies to Google Drive.

    Consumes tuples from data_queue. Stops when it receives None (sentinel).
    CSV filename: <ip>_<YYYYMMDD_HHMMSS>.csv
    """
    while True:
        item = data_queue.get()
        if item is None:
            data_queue.task_done()
            break
        ip, timestamp, samples = item
        filename = f"{ip}_{timestamp.strftime('%Y%m%d_%H%M%S')}.csv"
        try:
            saved = False
            try:
                with open(filename, 'w', newline='', encoding='utf-8') as f:
                    writer = csv.writer(f)
                    writer.writerow(CSV_COLUMNS)
                    writer.writerows(samples)
                saved = True
                logging.info(f"Saved {filename}")
            except Exception as e:
                logging.error(f"Failed to save {filename}: {e}")

            # The Drive copy is a separate concern: the gvfs mount is routinely
            # absent and gio may not be installed at all. Neither means the CSV
            # was lost — it is on local disk, and saying otherwise sends the
            # operator looking for data that is not missing.
            if saved:
                try:
                    destino = os.path.join(GDRIVE_PATH, filename)
                    subprocess.run(["gio", "copy", filename, destino], check=True)
                    logging.info(f"Copied {filename} to Google Drive")
                except Exception as e:
                    logging.error(f"Failed to copy {filename} to Google Drive: {e}")
        finally:
            data_queue.task_done()


def recv_exact(conn, n: int) -> bytes:
    """Read exactly n bytes from conn.

    A TCP recv() may return fewer bytes than asked for, and returns b'' for
    ever once the peer has closed — looping on that spins at 100% CPU without
    ever raising, so a closed peer aborts the read instead.

    Args:
        conn: connected socket
        n:    number of bytes to read; 0 returns b'' without touching the socket

    Returns:
        Exactly n bytes.

    Raises:
        ConnectionError: peer closed before n bytes arrived.
        socket.timeout:  propagated from recv — timeout policy is the caller's.
    """
    if n <= 0:
        return b''
    chunks    = []
    remaining = n
    while remaining > 0:
        chunk = conn.recv(remaining)
        if not chunk:
            raise ConnectionError(f'peer closed after {n - remaining}/{n} bytes')
        chunks.append(chunk)
        remaining -= len(chunk)
    return b''.join(chunks)


def handle_client(conn, addr, state: AppState):
    """Handle one ESP32 connection from start to finish.

    Reads the full sample payload, sends back the current ServerConfig from
    AppState, logs the connection to AppState, then queues the samples for
    CSV export. Called in a thread-pool worker by server_main.

    Args:
        conn:  accepted socket for this connection
        addr:  (ip, port) tuple of the remote device
        state: shared AppState — read for config, written for connection log
    """
    logging.info(f"Connected: {addr[0]}:{addr[1]}")
    conn.settimeout(CLIENT_TIMEOUT_SEC)
    samples = []

    try:
        # First 4 bytes: total number of sample bytes the device will send.
        # The header alone decides where the payload ends — counting bytes as
        # they arrive is what used to let the battery count as sample data.
        expected = int.from_bytes(recv_exact(conn, HEADER_SIZE_BYTES), byteorder='little')
        if expected > MAX_PAYLOAD_BYTES:
            raise ValueError(f"refusing {expected}-byte payload from {addr[0]}")

        payload = recv_exact(conn, expected)

        # A trailing partial frame is not a sample. Our firmware never sends one
        # (its ring buffer holds whole frames); an older build with a ring size
        # that is not a multiple of SAMPLE_SIZE_BYTES does.
        for i in range(expected // SAMPLE_SIZE_BYTES):
            start = i * SAMPLE_SIZE_BYTES
            samples.append(parse_sample(payload[start:start + SAMPLE_SIZE_BYTES]))

        # Battery is the last thing on the wire. Losing it must not cost the
        # device its config — same degradation as the production server.
        try:
            battery_mv = int.from_bytes(
                recv_exact(conn, BATTERY_SIZE_BYTES), byteorder='little')
        except (ConnectionError, socket.timeout):
            battery_mv = BATTERY_INVALID
            logging.warning(f"Battery reading not received from {addr[0]}")

        # Append battery reading to every sample row
        for s in samples:
            s.append(battery_mv)

        # Snapshot the config and claim the one-shot OTA arming in a single
        # operation. The response is built from that snapshot and never from a
        # second read of state.config: a POST /config landing in between would
        # otherwise ship a config no operator ever paired with this arming.
        config, ota = state.take_config_for_send()
        response = pack_server_config(
            config.sleep_min, config.idle_min,
            config.max_acq,   config.cooldown_sec, int(ota)
        )

        try:
            conn.sendall(response)
            logging.info(f"Config sent to {addr[0]} (update={int(ota)})")
        except OSError as e:
            # Nothing reached the device. Give the arming back so it goes to the
            # next device, and record the connection as not having taken it —
            # a history that lies here sends the operator hunting for an access
            # point that will never appear.
            if ota:
                state.rearm_ota()
                ota = False
            logging.error(f"Config not delivered to {addr[0]}: {e}")

        # Record this connection in the web UI history — also when the send
        # failed, because the samples did arrive.
        with state.lock:
            state.connections.append(ConnectionEntry(
                ip=addr[0],
                timestamp=datetime.datetime.now(),
                n_samples=len(samples),
                battery_mv=battery_mv,
                ota_sent=ota,
            ))

    except socket.timeout:
        logging.warning(f"Timeout from {addr[0]}")
    except Exception as e:
        logging.error(f"Error from {addr[0]}: {e}")
    finally:
        conn.close()
        # Queue samples for CSV export even if config send failed
        if samples:
            data_queue.put((addr[0], datetime.datetime.now(), samples))


def server_main(state: AppState):
    """Main TCP accept loop. Runs until the global `running` flag is cleared.

    Accepts connections on SERVER_IP:SERVER_PORT and dispatches each to
    handle_client in a thread-pool worker (up to 10 concurrent clients).
    Sends a sentinel to data_queue on exit so save_data can shut down cleanly.
    """
    global running
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.bind((SERVER_IP, SERVER_PORT))
    srv.listen(5)
    srv.settimeout(1.0)  # short timeout so the while-running check fires regularly
    logging.info(f"Server listening on {SERVER_IP}:{SERVER_PORT}")

    with ThreadPoolExecutor(max_workers=10) as executor:
        while running:
            try:
                conn, addr = srv.accept()
                executor.submit(handle_client, conn, addr, state)
            except socket.timeout:
                continue  # expected — just re-check running flag
            except Exception as e:
                logging.error(f"Server error: {e}")

    srv.close()
    data_queue.put(None)  # signal save_data to stop


def exit_monitor():
    """Waits for the user to type 'q' then sets running=False to stop the server."""
    global running
    print("Type 'q' to stop.")
    while True:
        if input().strip().lower() == 'q':
            running = False
            break


if __name__ == '__main__':
    from web.server import web_server_main

    state = AppState()
    threads = [
        threading.Thread(target=server_main,     args=(state,)),
        threading.Thread(target=web_server_main, args=(state,)),
        threading.Thread(target=exit_monitor),
        threading.Thread(target=save_data),
    ]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    print("Server stopped.")
