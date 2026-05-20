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
    SAMPLE_SIZE_BYTES, HEADER_SIZE_BYTES, BATTERY_SIZE_BYTES,
    SERVER_CONFIG_SIZE, SAMPLE_COLUMNS, parse_sample, pack_server_config
)
from config.settings import (
    SERVER_IP, SERVER_PORT, BUFFER_SIZE, GDRIVE_PATH,
    CLIENT_TIMEOUT_SEC, DEFAULT_SLEEP_MIN, DEFAULT_IDLE_MIN,
    DEFAULT_MAX_ACQ, DEFAULT_COOLDOWN_SEC
)

logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')

running    = True
data_queue = queue.Queue()


def save_data():
    while True:
        item = data_queue.get()
        if item is None:
            data_queue.task_done()
            break
        ip, timestamp, samples = item
        filename = f"{ip}_{timestamp.strftime('%Y%m%d_%H%M%S')}.csv"
        try:
            with open(filename, 'w', newline='', encoding='utf-8') as f:
                writer = csv.writer(f)
                writer.writerow(SAMPLE_COLUMNS + ['battery_mv'])
                writer.writerows(samples)
            destino = os.path.join(GDRIVE_PATH, filename)
            subprocess.run(["gio", "copy", filename, destino], check=True)
            logging.info(f"Saved and copied {filename}")
        except Exception as e:
            logging.error(f"Failed to save {filename}: {e}")
        finally:
            data_queue.task_done()


def handle_client(conn, addr):
    logging.info(f"Connected: {addr[0]}:{addr[1]}")
    conn.settimeout(CLIENT_TIMEOUT_SEC)
    buf     = b''
    samples = []

    try:
        header = conn.recv(HEADER_SIZE_BYTES)
        expected = int.from_bytes(header, byteorder='little')
        received = 0

        while running:
            data = conn.recv(BUFFER_SIZE)
            if not data:
                break
            buf      += data
            received += len(data)

            while len(buf) >= SAMPLE_SIZE_BYTES:
                samples.append(parse_sample(buf[:SAMPLE_SIZE_BYTES]))
                buf = buf[SAMPLE_SIZE_BYTES:]

            if received >= expected:
                # Read battery voltage
                while len(buf) < BATTERY_SIZE_BYTES:
                    buf += conn.recv(BATTERY_SIZE_BYTES - len(buf))
                battery_mv = int.from_bytes(buf[:BATTERY_SIZE_BYTES], byteorder='little')
                buf = buf[BATTERY_SIZE_BYTES:]

                # Append battery to every sample
                for s in samples:
                    s.append(battery_mv)

                # Send config response
                response = pack_server_config(
                    DEFAULT_SLEEP_MIN, DEFAULT_IDLE_MIN,
                    DEFAULT_MAX_ACQ,   DEFAULT_COOLDOWN_SEC
                )
                conn.sendall(response)
                logging.info(f"Config sent to {addr[0]}")
                break

    except socket.timeout:
        logging.warning(f"Timeout from {addr[0]}")
    except Exception as e:
        logging.error(f"Error from {addr[0]}: {e}")
    finally:
        conn.close()
        if samples:
            data_queue.put((addr[0], datetime.datetime.now(), samples))


def server_main():
    global running
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.bind((SERVER_IP, SERVER_PORT))
    srv.listen(5)
    srv.settimeout(1.0)
    logging.info(f"Server listening on {SERVER_IP}:{SERVER_PORT}")

    with ThreadPoolExecutor(max_workers=10) as executor:
        while running:
            try:
                conn, addr = srv.accept()
                executor.submit(handle_client, conn, addr)
            except socket.timeout:
                continue
            except Exception as e:
                logging.error(f"Server error: {e}")

    srv.close()
    data_queue.put(None)


def exit_monitor():
    global running
    print("Type 'q' to stop.")
    while True:
        if input().strip().lower() == 'q':
            running = False
            break


if __name__ == '__main__':
    threads = [
        threading.Thread(target=server_main),
        threading.Thread(target=exit_monitor),
        threading.Thread(target=save_data),
    ]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    print("Server stopped.")
