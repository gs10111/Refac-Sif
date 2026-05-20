# ESP32 Config Web UI — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Flask web UI on port 8080 that lets you edit the 4 ESP32 config values (`sleep_min`, `idle_min`, `max_acq`, `cooldown_sec`) without changing code, and shows the history of the last 50 device connections.

**Architecture:** A new `AppState` object (in-memory, thread-safe via `threading.Lock`) holds the current config and connection log. Flask runs in a dedicated thread alongside the existing TCP server. `handle_client` in `tcp_server.py` reads config from `AppState` and writes a `ConnectionEntry` on each successful exchange.

**Tech Stack:** Python 3, Flask 3.x, Jinja2 (bundled with Flask), pytest

---

## File Map

| Action | Path | Responsibility |
|--------|------|----------------|
| Create | `backend/app_state.py` | `DeviceConfig`, `ConnectionEntry`, `AppState` |
| Create | `backend/web/__init__.py` | package marker |
| Create | `backend/web/server.py` | Flask app factory + routes |
| Create | `backend/web/templates/index.html` | split UI: config left, history right |
| Create | `backend/tests/__init__.py` | package marker |
| Create | `backend/tests/conftest.py` | sys.path setup |
| Create | `backend/tests/test_app_state.py` | unit tests for AppState |
| Create | `backend/tests/test_web_server.py` | unit tests for Flask routes |
| Create | `backend/tests/test_tcp_server.py` | integration tests for handle_client changes |
| Modify | `backend/server/tcp_server.py` | accept AppState, read config, log connection |
| Modify | `backend/requirements.txt` | add flask, pytest |

---

## Task 1: Bootstrap test environment

**Files:**
- Modify: `backend/requirements.txt`
- Create: `backend/tests/__init__.py`
- Create: `backend/tests/conftest.py`

- [ ] **Step 1.1: Update requirements.txt**

Replace the current content with:

```
numpy
flask>=3.0
pytest>=8.0
```

- [ ] **Step 1.2: Install dependencies**

Run from the `backend/` directory:

```bash
cd /home/gabriel/repos/Refactor-Sif/backend
pip install -r requirements.txt
```

Expected: packages install without errors.

- [ ] **Step 1.3: Create test package**

Create `backend/tests/__init__.py` — empty file.

- [ ] **Step 1.4: Create conftest.py**

Create `backend/tests/conftest.py`:

```python
import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
```

- [ ] **Step 1.5: Verify pytest finds tests**

```bash
cd /home/gabriel/repos/Refactor-Sif/backend
pytest tests/ -v
```

Expected: `no tests ran` (0 collected) — no error.

- [ ] **Step 1.6: Commit**

```bash
cd /home/gabriel/repos/Refactor-Sif
git add backend/requirements.txt backend/tests/
git commit -m "chore: add flask and pytest to requirements, create tests package"
```

---

## Task 2: AppState module (TDD)

**Files:**
- Create: `backend/app_state.py`
- Create: `backend/tests/test_app_state.py`

- [ ] **Step 2.1: Write the failing tests**

Create `backend/tests/test_app_state.py`:

```python
import threading
from app_state import AppState, DeviceConfig, ConnectionEntry
import datetime


def test_default_config_matches_settings():
    from config.settings import (
        DEFAULT_SLEEP_MIN, DEFAULT_IDLE_MIN,
        DEFAULT_MAX_ACQ, DEFAULT_COOLDOWN_SEC,
    )
    state = AppState()
    assert state.config.sleep_min    == DEFAULT_SLEEP_MIN
    assert state.config.idle_min     == DEFAULT_IDLE_MIN
    assert state.config.max_acq      == DEFAULT_MAX_ACQ
    assert state.config.cooldown_sec == DEFAULT_COOLDOWN_SEC


def test_config_fields_are_mutable():
    state = AppState()
    with state.lock:
        state.config.sleep_min = 99
    assert state.config.sleep_min == 99


def test_connections_starts_empty():
    state = AppState()
    assert len(state.connections) == 0


def test_connections_append_and_order():
    state = AppState()
    entry = ConnectionEntry(
        ip='10.0.0.1',
        timestamp=datetime.datetime.now(),
        n_samples=10,
        battery_mv=3800,
    )
    with state.lock:
        state.connections.append(entry)
    assert len(state.connections) == 1
    assert state.connections[0].ip == '10.0.0.1'
    assert state.connections[0].n_samples == 10


def test_connections_max_50():
    state = AppState()
    for i in range(60):
        state.connections.append(ConnectionEntry(
            ip=f'10.0.0.{i}',
            timestamp=datetime.datetime.now(),
            n_samples=i,
            battery_mv=3800,
        ))
    assert len(state.connections) == 50


def test_lock_is_threading_lock():
    state = AppState()
    assert isinstance(state.lock, type(threading.Lock()))
```

- [ ] **Step 2.2: Run tests to verify they fail**

```bash
cd /home/gabriel/repos/Refactor-Sif/backend
pytest tests/test_app_state.py -v
```

Expected: `ModuleNotFoundError: No module named 'app_state'`

- [ ] **Step 2.3: Implement app_state.py**

Create `backend/app_state.py`:

```python
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
```

- [ ] **Step 2.4: Run tests to verify they pass**

```bash
cd /home/gabriel/repos/Refactor-Sif/backend
pytest tests/test_app_state.py -v
```

Expected: 6 tests PASSED.

- [ ] **Step 2.5: Commit**

```bash
cd /home/gabriel/repos/Refactor-Sif
git add backend/app_state.py backend/tests/test_app_state.py
git commit -m "feat: add AppState with DeviceConfig and ConnectionEntry"
```

---

## Task 3: Flask web server + HTML template (TDD)

**Files:**
- Create: `backend/web/__init__.py`
- Create: `backend/web/server.py`
- Create: `backend/web/templates/index.html`
- Create: `backend/tests/test_web_server.py`

- [ ] **Step 3.1: Write the failing tests**

Create `backend/tests/test_web_server.py`:

```python
import pytest
import datetime
from app_state import AppState, ConnectionEntry
from web.server import create_app


@pytest.fixture
def state():
    return AppState()


@pytest.fixture
def client(state):
    app = create_app(state)
    app.config['TESTING'] = True
    return app.test_client(), state


def test_get_index_returns_200(client):
    c, _ = client
    response = c.get('/')
    assert response.status_code == 200


def test_get_index_shows_default_config(client):
    c, state = client
    response = c.get('/')
    body = response.data.decode()
    assert str(state.config.sleep_min)    in body
    assert str(state.config.idle_min)     in body
    assert str(state.config.max_acq)      in body
    assert str(state.config.cooldown_sec) in body


def test_post_config_updates_state(client):
    c, state = client
    response = c.post('/config', data={
        'sleep_min':    '120',
        'idle_min':     '10',
        'max_acq':      '3',
        'cooldown_sec': '2',
    })
    assert response.status_code in (301, 302)
    assert state.config.sleep_min    == 120
    assert state.config.idle_min     == 10
    assert state.config.max_acq      == 3
    assert state.config.cooldown_sec == 2


def test_post_config_rejects_non_integer(client):
    c, state = client
    original = state.config.sleep_min
    response = c.post('/config', data={
        'sleep_min':    'abc',
        'idle_min':     '10',
        'max_acq':      '3',
        'cooldown_sec': '2',
    })
    assert response.status_code == 400
    assert state.config.sleep_min == original


def test_post_config_rejects_zero(client):
    c, state = client
    original = state.config.sleep_min
    response = c.post('/config', data={
        'sleep_min':    '0',
        'idle_min':     '10',
        'max_acq':      '3',
        'cooldown_sec': '2',
    })
    assert response.status_code == 400
    assert state.config.sleep_min == original


def test_get_index_shows_connection_history(client):
    c, state = client
    state.connections.append(ConnectionEntry(
        ip='192.168.1.5',
        timestamp=datetime.datetime(2026, 5, 20, 14, 0, 0),
        n_samples=100,
        battery_mv=3750,
    ))
    response = c.get('/')
    body = response.data.decode()
    assert '192.168.1.5' in body
    assert '100' in body
    assert '3750' in body
```

- [ ] **Step 3.2: Run tests to verify they fail**

```bash
cd /home/gabriel/repos/Refactor-Sif/backend
pytest tests/test_web_server.py -v
```

Expected: `ModuleNotFoundError: No module named 'web'`

- [ ] **Step 3.3: Create web package marker**

Create `backend/web/__init__.py` — empty file.

- [ ] **Step 3.4: Implement Flask server**

Create `backend/web/server.py`:

```python
from flask import Flask, render_template, request, redirect

from app_state import AppState


def create_app(state: AppState) -> Flask:
    app = Flask(__name__)

    @app.route('/')
    def index():
        with state.lock:
            config = state.config
            connections = list(state.connections)
        return render_template('index.html', config=config, connections=connections)

    @app.route('/config', methods=['POST'])
    def update_config():
        try:
            sleep_min    = int(request.form['sleep_min'])
            idle_min     = int(request.form['idle_min'])
            max_acq      = int(request.form['max_acq'])
            cooldown_sec = int(request.form['cooldown_sec'])
        except (KeyError, ValueError):
            return 'Valores invalidos: todos os campos devem ser inteiros.', 400

        if any(v <= 0 for v in [sleep_min, idle_min, max_acq, cooldown_sec]):
            return 'Valores invalidos: todos os campos devem ser maiores que zero.', 400

        with state.lock:
            state.config.sleep_min    = sleep_min
            state.config.idle_min     = idle_min
            state.config.max_acq      = max_acq
            state.config.cooldown_sec = cooldown_sec

        return redirect('/')


def web_server_main(state: AppState, port: int = 8080) -> None:
    app = create_app(state)
    app.run(host='0.0.0.0', port=port, use_reloader=False)
```

- [ ] **Step 3.5: Create HTML template**

Create `backend/web/templates/index.html`:

```html
<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="UTF-8">
  <title>ESP32 Config Server</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      background: #1a1a2e; color: #e0e0e0;
      font-family: monospace; padding: 24px;
      min-height: 100vh;
    }
    h1 { color: #7ecfff; margin-bottom: 24px; font-size: 1.1rem; letter-spacing: 1px; }
    .layout { display: flex; gap: 24px; align-items: flex-start; }
    .panel-config  { flex: 1; }
    .panel-history { flex: 2; }
    h2 {
      color: #7ecfff; font-size: 0.75rem; text-transform: uppercase;
      letter-spacing: 2px; margin-bottom: 14px;
    }
    .field {
      display: flex; justify-content: space-between; align-items: center;
      background: #0f3460; padding: 10px 14px; border-radius: 4px;
      margin-bottom: 8px;
    }
    .field label { color: #aaa; font-size: 0.85rem; }
    .field input {
      width: 72px; background: #1a1a2e; border: 1px solid #7ecfff;
      color: #7ecfff; text-align: center; padding: 5px;
      border-radius: 3px; font-size: 0.95rem; font-family: monospace;
    }
    button {
      width: 100%; margin-top: 14px; background: #e94560; color: white;
      border: none; padding: 10px; border-radius: 4px; cursor: pointer;
      font-size: 0.85rem; font-family: monospace; letter-spacing: 2px;
    }
    button:hover { background: #c73652; }
    table { width: 100%; border-collapse: collapse; font-size: 0.82rem; }
    th {
      color: #aaa; text-align: left; padding: 7px 10px;
      border-bottom: 1px solid #0f3460; font-weight: normal;
    }
    td { padding: 7px 10px; border-bottom: 1px solid #111830; }
    tr:hover td { background: #0f3460; }
    .empty { color: #444; font-style: italic; padding: 14px 10px; }
  </style>
</head>
<body>
  <h1>ESP32 CONFIG SERVER</h1>
  <div class="layout">

    <div class="panel-config">
      <h2>Configuracao</h2>
      <form method="POST" action="/config">
        <div class="field">
          <label>Sleep (min)</label>
          <input type="number" name="sleep_min" value="{{ config.sleep_min }}" min="1" required>
        </div>
        <div class="field">
          <label>Idle (min)</label>
          <input type="number" name="idle_min" value="{{ config.idle_min }}" min="1" required>
        </div>
        <div class="field">
          <label>Max Acq</label>
          <input type="number" name="max_acq" value="{{ config.max_acq }}" min="1" required>
        </div>
        <div class="field">
          <label>Cooldown (s)</label>
          <input type="number" name="cooldown_sec" value="{{ config.cooldown_sec }}" min="1" required>
        </div>
        <button type="submit">SALVAR</button>
      </form>
    </div>

    <div class="panel-history">
      <h2>Ultimas Conexoes</h2>
      <table>
        <thead>
          <tr>
            <th>IP</th>
            <th>Horario</th>
            <th>Amostras</th>
            <th>Bateria (mV)</th>
          </tr>
        </thead>
        <tbody>
          {% if connections %}
            {% for entry in connections | reverse %}
            <tr>
              <td>{{ entry.ip }}</td>
              <td>{{ entry.timestamp.strftime('%Y-%m-%d %H:%M:%S') }}</td>
              <td>{{ entry.n_samples }}</td>
              <td>{{ entry.battery_mv }}</td>
            </tr>
            {% endfor %}
          {% else %}
            <tr><td colspan="4" class="empty">Nenhuma conexao registrada ainda.</td></tr>
          {% endif %}
        </tbody>
      </table>
    </div>

  </div>
</body>
</html>
```

- [ ] **Step 3.6: Run tests to verify they pass**

```bash
cd /home/gabriel/repos/Refactor-Sif/backend
pytest tests/test_web_server.py -v
```

Expected: 6 tests PASSED.

- [ ] **Step 3.7: Commit**

```bash
cd /home/gabriel/repos/Refactor-Sif
git add backend/web/ backend/tests/test_web_server.py
git commit -m "feat: add Flask web server with config form and connection history"
```

---

## Task 4: Modify tcp_server.py to use AppState (TDD)

**Files:**
- Modify: `backend/server/tcp_server.py`
- Create: `backend/tests/test_tcp_server.py`

- [ ] **Step 4.1: Write the failing tests**

Create `backend/tests/test_tcp_server.py`:

```python
import struct
import datetime
from unittest.mock import MagicMock, patch

import pytest

from app_state import AppState, DeviceConfig
from protocol.packet import (
    HEADER_SIZE_BYTES, SAMPLE_SIZE_BYTES, BATTERY_SIZE_BYTES,
)
from server.tcp_server import handle_client


BATTERY_MV = 3800


def make_recv_sequence(n_samples: int = 1):
    """Return list of bytes objects for conn.recv side_effect."""
    body   = bytes(SAMPLE_SIZE_BYTES * n_samples)
    header = len(body).to_bytes(HEADER_SIZE_BYTES, 'little')
    batt   = BATTERY_MV.to_bytes(BATTERY_SIZE_BYTES, 'little')
    return [header, body, batt]


def test_handle_client_sends_config_from_state():
    """Config values in AppState are used in the response to ESP32."""
    state = AppState()
    state.config.sleep_min    = 99
    state.config.idle_min     = 11
    state.config.max_acq      = 4
    state.config.cooldown_sec = 7

    conn = MagicMock()
    conn.recv.side_effect = make_recv_sequence(n_samples=1)

    handle_client(conn, ('10.0.0.1', 5000), state)

    sent = conn.sendall.call_args[0][0]
    sleep_min, idle_min, max_acq, cooldown_sec = struct.unpack('<HHHH', sent)
    assert sleep_min    == 99
    assert idle_min     == 11
    assert max_acq      == 4
    assert cooldown_sec == 7


def test_handle_client_logs_connection_to_state():
    """After a successful exchange, a ConnectionEntry is added to state."""
    state = AppState()

    conn = MagicMock()
    conn.recv.side_effect = make_recv_sequence(n_samples=2)

    handle_client(conn, ('10.0.0.2', 5001), state)

    assert len(state.connections) == 1
    entry = state.connections[0]
    assert entry.ip         == '10.0.0.2'
    assert entry.n_samples  == 2
    assert entry.battery_mv == BATTERY_MV
    assert isinstance(entry.timestamp, datetime.datetime)


def test_handle_client_does_not_log_on_timeout():
    """A socket timeout produces no ConnectionEntry."""
    import socket
    state = AppState()

    conn = MagicMock()
    conn.recv.side_effect = socket.timeout

    handle_client(conn, ('10.0.0.3', 5002), state)

    assert len(state.connections) == 0
```

- [ ] **Step 4.2: Run tests to verify they fail**

```bash
cd /home/gabriel/repos/Refactor-Sif/backend
pytest tests/test_tcp_server.py -v
```

Expected: `TypeError: handle_client() takes 2 positional arguments but 3 were given`

- [ ] **Step 4.3: Modify tcp_server.py**

Replace the current `backend/server/tcp_server.py` with:

```python
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
from app_state import AppState, ConnectionEntry

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


def handle_client(conn, addr, state: AppState):
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
                while len(buf) < BATTERY_SIZE_BYTES:
                    buf += conn.recv(BATTERY_SIZE_BYTES - len(buf))
                battery_mv = int.from_bytes(buf[:BATTERY_SIZE_BYTES], byteorder='little')
                buf = buf[BATTERY_SIZE_BYTES:]

                for s in samples:
                    s.append(battery_mv)

                with state.lock:
                    cfg = state.config
                response = pack_server_config(
                    cfg.sleep_min, cfg.idle_min,
                    cfg.max_acq,   cfg.cooldown_sec
                )
                conn.sendall(response)
                logging.info(f"Config sent to {addr[0]}")

                with state.lock:
                    state.connections.append(ConnectionEntry(
                        ip=addr[0],
                        timestamp=datetime.datetime.now(),
                        n_samples=len(samples),
                        battery_mv=battery_mv,
                    ))
                break

    except socket.timeout:
        logging.warning(f"Timeout from {addr[0]}")
    except Exception as e:
        logging.error(f"Error from {addr[0]}: {e}")
    finally:
        conn.close()
        if samples:
            data_queue.put((addr[0], datetime.datetime.now(), samples))


def server_main(state: AppState):
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
                executor.submit(handle_client, conn, addr, state)
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
```

- [ ] **Step 4.4: Run all tests**

```bash
cd /home/gabriel/repos/Refactor-Sif/backend
pytest tests/ -v
```

Expected: all tests PASSED (15 total).

- [ ] **Step 4.5: Commit**

```bash
cd /home/gabriel/repos/Refactor-Sif
git add backend/server/tcp_server.py backend/tests/test_tcp_server.py
git commit -m "feat: wire AppState into tcp_server — config from state, log connections"
```

---

## Task 5: Manual smoke test

- [ ] **Step 5.1: Start the server**

```bash
cd /home/gabriel/repos/Refactor-Sif/backend
python -m server.tcp_server
```

Expected: log lines:
```
INFO - Server listening on 0.0.0.0:12345
 * Running on http://0.0.0.0:8080
```

- [ ] **Step 5.2: Open the web UI**

Open `http://localhost:8080` in the browser.

Expected: page with config form (sleep=240, idle=20, max_acq=5, cooldown=5) on the left and empty history table on the right.

- [ ] **Step 5.3: Change a value and save**

Change `sleep_min` to `30`, click SALVAR.

Expected: page reloads showing sleep=30. Other fields unchanged.

- [ ] **Step 5.4: Verify invalid input is rejected**

Change `sleep_min` to `abc`, click SALVAR.

Expected: browser shows `Valores invalidos: todos os campos devem ser inteiros.` (HTTP 400). Config not changed.

- [ ] **Step 5.5: Stop the server**

Type `q` + Enter in the terminal.

Expected: `Server stopped.`
