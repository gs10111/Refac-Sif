# Backend — Auditoria do refactor + restauração do campo `update` (OTA)

**Data:** 2026-07-27
**Autor:** sifbe-sama (backend)
**Status:** DESIGN — nenhum código alterado. Aguarda go-ahead do lead (@lave).
**Escopo de propriedade:** `backend/**`, `docs/backend/**`. Nada em `src/`, `lib/`, `include/`, `platformio.ini`, `docs/qa/`.

Baseline atual: `cd backend && ./.venv/bin/python -m pytest tests/ -q` => 16 passed
(não executado por mim — shell read-only; valor informado no briefing).

---

## 1. Auditoria: `backend/` vs servidores originais

Referências:
- `pyFiles/win_server.py` — envia **10 bytes / 5 campos** (com `update`)
- `pyFiles/server_lix_csv2.py` — envia **8 bytes / 4 campos** (sem `update`), tem cópia gdrive
- `pyFiles/linux_server.py` — legado (amostra de 14 B, sem header, sem bateria) — só histórico
- Refactor: `backend/server/tcp_server.py`, `backend/protocol/packet.py`

Os dois originais **divergem entre si** no tamanho da resposta. O contrato decidido (10 B) é o do `win_server.py`.

### 1.1 Tabela de diferenças comportamentais

| Aspecto | Original (`win_server` / `server_lix_csv2`) | Refactor | Veredito |
|---|---|---|---|
| Leitura do header | `conn.recv(4)` **fora** do `try` — exceção mata a thread sem `conn.close()` | `tcp_server.py:94`, dentro do `try` | refactor melhor |
| Header curto (<4 B) | não trata | não trata (`tcp_server.py:94-95`) | **bug herdado** (B2) |
| Contagem `received >= expected` | conta só bytes do loop, header fora | idem (`tcp_server.py:96,103,110`) | equivalente, correto |
| Parse de frames | consome todo múltiplo de 18 do buffer | idem (`tcp_server.py:106-108`) | **bug** quando `expected % 18 != 0` (B1) |
| Leitura da bateria | pega `min(2, len(buffer))`; se <2 → `battery=-1` e **segue** enviando config | `while len(buf) < 2: buf += conn.recv(...)` (`tcp_server.py:112-113`) | **bug**: loop infinito + perda de config (B3) |
| Resposta ao device | 10 B / 5 campos (`win_server.py:114`) ou 8 B / 4 (`server_lix_csv2.py:124`) | 8 B / 4 campos (`packet.py:17`) | **campo `update` perdido** (B0) |
| Fonte dos valores | hardcoded no código | `AppState` editável pela UI web | refactor melhor |
| Coluna bateria no CSV | anexada no `finally`; `UnboundLocalError` se timeout antes | anexada antes do `sendall` (`tcp_server.py:118-119`) | refactor melhor, mas CSV fica **irregular** em transferência truncada (B4) |
| Nomes das colunas CSV | `timestamp,x_data,x_gyro,y_data,y_gyro,z_data,z_gyro,temp,battery_voltage` | `timestamp,accel_x,gyro_x,accel_y,gyro_y,accel_z,gyro_z,temp,battery_mv` (`packet.py:9`, `tcp_server.py:64`) | **regressão de pipeline** (B5) |
| Cópia gdrive | bloco `except` próprio; erro de cópia ≠ erro de escrita | `subprocess.run(check=True)` no mesmo `try` do arquivo (`tcp_server.py:62-70`) | **log enganoso** (B6) |
| `task_done()` | chamado antes da cópia (some se a escrita falha) | `finally` (`tcp_server.py:71-72`) | refactor melhor |
| Timeout socket | 6.0 s por `recv` | 6.0 s (`CLIENT_TIMEOUT_SEC`) | igual |
| Histórico web | não existe | `ConnectionEntry` em `AppState` | novo |
| Registro no histórico | — | **depois** do `sendall` (`tcp_server.py:127-137`) | conexão válida some do histórico se o envio falha (B7) |
| Limite do `expected` | nenhum | nenhum | **bug herdado** (B8) |

### 1.2 Lista de bugs (ordem de severidade)

**B0 — `update`/OTA perdido no refactor. CRÍTICO (funcional).**
`backend/protocol/packet.py:16-17` empacota `'<HHHH'` (8 B). O original `pyFiles/win_server.py:113-114` empacota `'<HHHHH'` com `update`.
Efeito: não existe mais nenhuma forma de colocar o device em modo OTA. É o objeto do item 2 deste documento.

**B1 — Bateria engolida como amostra quando o payload não fecha em múltiplo de 18. CRÍTICO.**
`backend/server/tcp_server.py:106-113`.
O loop de frames roda **antes** da checagem `received >= expected` e consome qualquer bloco de 18 B do buffer, sem saber onde termina a região de amostras.
Caso determinístico com a decisão **D2 (buffer = 700000 bytes)**: quando o ring buffer dá wrap, o firmware envia `totalAcqBytes = byteBufferSize = 700000`. Ora, `700000 = 38888 × 18 + 16`. Sobram **16 bytes** de frame parcial; somados aos **2 bytes de bateria** dão exatamente **18** → o parser cria **uma amostra falsa feita de lixo + os bytes da bateria**, o buffer fica vazio, e a leitura da bateria (linha 112) bloqueia esperando 2 bytes que nunca chegam → `socket.timeout` (6 s) → **config nunca é enviada**, **nenhum `ConnectionEntry`**, e o CSV vai para a fila **sem** a coluna de bateria (ver B4).
Para qualquer `expected % 18` entre 1 e 15 o efeito é mais silencioso: `buf[:2]` são **bytes de amostra**, e a bateria gravada no CSV e no histórico é **lixo**.
Original: degradava para `battery = -1` e **ainda assim respondia** ao device (`win_server.py:97-117`).

**B2 — Header parcial não tratado. ALTO.**
`backend/server/tcp_server.py:94-95`. `conn.recv(4)` pode retornar 1..3 bytes (TCP pode fragmentar; o firmware faz um `write()` separado, mas Nagle/MTU não garantem nada). `int.from_bytes` de 2 bytes produz um `expected` truncado → o servidor responde cedo demais e desalinha todo o resto da conexão. Bug herdado dos originais.

**B3 — Loop infinito na leitura da bateria quando o peer fecha. ALTO (DoS).**
`backend/server/tcp_server.py:112-113`:
```python
while len(buf) < BATTERY_SIZE_BYTES:
    buf += conn.recv(BATTERY_SIZE_BYTES - len(buf))
```
Em socket fechado `recv` retorna `b''` **imediatamente e para sempre** (não levanta timeout). O `while` nunca termina → worker do `ThreadPoolExecutor` girando a 100 % de CPU indefinidamente. Com `max_workers=10` (`tcp_server.py:165`), **10 quedas de link durante o envio da bateria travam o servidor permanentemente**: novas conexões são aceitas e nunca processadas.

**B4 — CSV irregular em transferência truncada. MÉDIO.**
`backend/server/tcp_server.py:144-148`. Se a exceção acontece antes da linha 118, as linhas de `samples` têm 8 colunas, mas o cabeçalho escrito em `tcp_server.py:64` tem 9 (`SAMPLE_COLUMNS + ['battery_mv']`). O CSV sai desalinhado, e `pandas.read_csv` do script de análise lê a última coluna como `NaN` sem avisar.

**B5 — Nomes das colunas do CSV mudaram: quebra o script de análise. MÉDIO/ALTO (pipeline de dados).**
`backend/protocol/packet.py:9` usa `accel_x/gyro_x/...`; `backend/tools/analysis/cliente_local_csv.py:107-110` lê `df['x_data']`, `df['x_gyro']`, `df['y_data']`, ... e `tcp_server.py:64` grava `battery_mv` em vez de `battery_voltage`.
Efeito: **todo CSV gerado pelo servidor refatorado quebra o script de análise com `KeyError`**, e não casa com o corpus histórico de CSVs já coletados. A ordem das colunas está correta; só os nomes mudaram.
→ Decisão de produto (ver §6, Q1).

**B6 — Erro de cópia para o Google Drive é reportado como falha de salvamento. BAIXO/MÉDIO.**
`backend/server/tcp_server.py:61-70`: `open/write` e `subprocess.run(["gio","copy",...], check=True)` estão no mesmo `try` com `except Exception` genérico, que loga `Failed to save {filename}`. Se o gvfs não estiver montado (caso comum), **o CSV local foi salvo com sucesso** mas o operador lê "falha ao salvar" e pode concluir que perdeu a aquisição. Os originais separavam os `except` (`server_lix_csv2.py:55-60`).
Adicional: a cópia é síncrona numa única thread `save_data`; um gvfs travado segura a fila inteira.

**B7 — Conexão bem-sucedida some do histórico se o `sendall` falha. BAIXO.**
`backend/server/tcp_server.py:127-137`: o `ConnectionEntry` é acrescentado **depois** do `conn.sendall`. Se o device fecha o link logo após enviar os dados (comportamento plausível quando o WiFi da planta oscila), os dados foram recebidos e vão para o CSV, mas a conexão **não aparece** na UI web. Piora com o OTA: o operador não vê que aquela transmissão ocorreu.

**B8 — `expected` do device não é validado. BAIXO (robustez).**
`backend/server/tcp_server.py:95`. Header corrompido (`0xFFFFFFFF`) → 4 GiB esperados; o servidor fica acumulando em `buf` até o peer fechar. Sem limite superior. Bug herdado.

**B9 — Constantes mortas. TRIVIAL.**
`SERVER_CONFIG_SIZE` é importado em `backend/server/tcp_server.py:34` e nunca usado. `RESPONSE_TIMEOUT_SEC` (`backend/config/settings.py:13`) não é usado em lugar nenhum — a espera pela resposta é do lado do firmware (`src/services/connectivity/tcp_client.cpp:32`, 5000 ms).

### 1.3 Correção proposta para B1/B2/B3/B4/B8 — um único helper

Todos esses bugs são o mesmo defeito: **o servidor não lê quantidades exatas**. Proposta:

```python
def recv_exact(conn, n: int) -> bytes:
    """Read exactly n bytes. Raises ConnectionError if the peer closes first."""
    buf = b''
    while len(buf) < n:
        chunk = conn.recv(min(BUFFER_SIZE, n - len(buf)))
        if not chunk:
            raise ConnectionError(f'peer closed after {len(buf)}/{n} bytes')
        buf += chunk
    return buf
```

e o `handle_client` passa a ser:

```python
header   = recv_exact(conn, HEADER_SIZE_BYTES)            # corrige B2
expected = int.from_bytes(header, 'little')
if expected > MAX_PAYLOAD_BYTES:                          # corrige B8
    raise ValueError(...)
payload  = recv_exact(conn, expected)                     # corrige B1 (fronteira exata)
n_frames = expected // SAMPLE_SIZE_BYTES                  # o resto (16 B no wrap) é descartado
samples  = [parse_sample(payload[i*18:(i+1)*18]) for i in range(n_frames)]
try:
    battery_mv = int.from_bytes(recv_exact(conn, BATTERY_SIZE_BYTES), 'little')
except (ConnectionError, socket.timeout):
    battery_mv = BATTERY_INVALID   # -1, igual ao original; a config ainda é enviada
```
e a coluna de bateria é anexada **sempre** (`BATTERY_INVALID` no caminho de erro) antes de enfileirar o CSV → corrige **B4**.
Custo de memória: `expected` ≤ 700000 B em RAM por conexão; a lista `samples` já era muito maior que isso. Aceitável.
`MAX_PAYLOAD_BYTES` sugerido: `2 * 700000 = 1400000` (constante em `config/settings.py`).

---

## 2. Restauração do campo `update` — mudanças exatas

### 2.1 Contrato de fio (decidido pelo lead, não alterar sem avisar)

```
ESP32 -> servidor (por conexão):
  [4 B]  uint32 LE  total_sample_bytes
  [N B]  amostras, 18 B cada
  [2 B]  uint16 LE  battery_mv

servidor -> ESP32:  10 BYTES, 5 x uint16 little-endian
  offset 0 : sleep_min     (default 240)
  offset 2 : idle_min      (default  20)
  offset 4 : max_acq       (default   5)
  offset 6 : cooldown_sec  (default   5)
  offset 8 : update        (0 ou 1)
```

Golden bytes com os defaults e `update=1`:
`b'\xf0\x00\x14\x00\x05\x00\x05\x00\x01\x00'`
Idêntico ao que `pyFiles/win_server.py:114` já emite → o servidor antigo continua **wire-compatible** com o firmware novo.

### 2.2 `backend/protocol/packet.py`

```python
SERVER_CONFIG_SIZE = 10   # era 8 — 5 x uint16_t

def pack_server_config(sleep_min, idle_min, max_acq, cooldown_sec, update) -> bytes:
    if update not in (0, 1):
        raise ValueError('update must be 0 or 1')
    return struct.pack('<HHHHH', sleep_min, idle_min, max_acq, cooldown_sec, update)
```
`update` **sem valor default** — de propósito: força todo call site a ser explícito e faz o teste antigo falhar de forma visível em vez de mandar `0` silenciosamente.

### 2.3 `backend/config/settings.py`

```python
DEFAULT_UPDATE     = 0        # OTA desarmado ao subir o servidor
MAX_PAYLOAD_BYTES  = 1400000  # 2 x 700000 (D2) — teto de sanidade para o header
BATTERY_INVALID    = -1       # mesma sentinela do original
```
`RESPONSE_TIMEOUT_SEC` é removido (morto, B9).

### 2.4 `backend/app_state.py`

`DeviceConfig` **não** ganha o campo: `update` não é configuração persistente do device, é um **armamento de uso único** do servidor. Fica em `AppState`:

```python
@dataclass
class ConnectionEntry:
    ip:         str
    timestamp:  datetime.datetime
    n_samples:  int
    battery_mv: int
    ota_sent:   bool = False      # NOVO — este device recebeu update=1

class AppState:
    def __init__(self):
        self.config      = DeviceConfig()
        self.connections = deque(maxlen=50)
        self.ota_armed   = bool(DEFAULT_UPDATE)   # NOVO
        self.lock        = threading.Lock()

    def take_config_for_send(self) -> tuple[DeviceConfig, bool]:
        """Snapshot da config + CLAIM atômico do OTA one-shot.

        Retorna (cópia da config, ota). Se ota=True o flag JÁ foi limpo:
        só a primeira conexão a chamar isto recebe True.
        """
        with self.lock:
            ota = self.ota_armed
            self.ota_armed = False
            return dataclasses.replace(self.config), ota

    def rearm_ota(self) -> None:
        """Compensação: o envio falhou, ninguém recebeu o flag."""
        with self.lock:
            self.ota_armed = True
```

`take_config_for_send` faz leitura da config e claim do OTA **na mesma tomada de lock** — sem isso, dois devices conectados simultaneamente poderiam ambos ler `ota_armed=True` e ambos entrariam em OTA.

### 2.5 `backend/server/tcp_server.py`

```python
config, ota = state.take_config_for_send()
response = pack_server_config(
    config.sleep_min, config.idle_min, config.max_acq, config.cooldown_sec,
    1 if ota else 0,
)
try:
    conn.sendall(response)
except OSError:
    if ota:
        state.rearm_ota()      # ninguém recebeu — devolve o armamento
    raise
logging.info(f'Config sent to {addr[0]} (update={int(ota)})')

with state.lock:
    state.connections.append(ConnectionEntry(..., ota_sent=ota))
```
Junto: mover o `ConnectionEntry` para **antes** do `sendall` não resolve (não saberíamos se o flag foi entregue); a correção de **B7** é registrar a entrada também no caminho de erro, com `ota_sent=False`.
O docstring do módulo (`tcp_server.py:11-13`) muda de `[8 bytes]`/4 campos para `[10 bytes]`/5 campos.

### 2.6 `backend/web/server.py`

```python
@app.route('/config', methods=['POST'])
def update_config():
    ...validação dos 4 inteiros (inalterada)...
    ota = 'ota_armed' in request.form      # checkbox: só chega no POST quando marcado
    with state.lock:
        state.config.sleep_min = ...
        state.ota_armed = ota
    return redirect('/', 303)
```
`GET /` passa `ota_armed=state.ota_armed` (lido dentro do lock, junto da cópia da config) para o template.
Nota: entrada inválida (400) **não** pode alterar `ota_armed` — o `return` de erro acontece antes de qualquer escrita, o que já é o caso hoje.

### 2.7 `backend/web/templates/index.html`

- Painel esquerdo, abaixo de "Cooldown (s)":
  `<input type="checkbox" name="ota_armed" {% if ota_armed %}checked{% endif %}>` com label **"OTA na próxima transmissão"** + badge de estado (`ARMADO` em `#e94560` / `desarmado` em cinza).
- Aviso curto ao lado quando armado: *"o próximo device que transmitir vai reiniciar em modo AP"*.
- Tabela de histórico ganha a coluna **OTA** (`SIM` quando `entry.ota_sent`, `-` caso contrário) e o `colspan` da linha vazia passa de 4 para 5.

### 2.8 Lado firmware (informativo — dono: sifemb-gomi, eu não edito)

`lib/protocol/packet.h`: `struct ServerConfig` ganha `uint16_t update;` como **quinto e último** campo (`packed`, 10 B).
`src/services/connectivity/tcp_client.cpp:31`: a espera `_client.available() < sizeof(ServerConfig)` passa a exigir 10 bytes automaticamente — mas o timeout de 5000 ms segue válido.
Fluxo OTA do original a reproduzir: `main.cpp:282-289` (grava `Preferences("config").putBool("update", true)` + `ESP.restart()`) e `main.cpp:73-96` (no boot, se o flag está setado: limpa o flag, sobe `WIFI_AP` `"Update driver - <MAC>"` / `12345678`, `configureOtaServer()`, timeout de 5 min → `ESP.restart()`).

---

## 3. UX e semântica do armamento OTA

### 3.1 Decisão vigente: (a) one-shot na próxima conexão — **D3, binding**

O operador marca "OTA na próxima transmissão"; o servidor manda `update=1` para o **próximo** device que completar uma transmissão e limpa o flag na mesma operação atômica. O device seguinte recebe `update=0`.

### 3.2 Comparação das três semânticas

| | (a) one-shot **[decidido]** | (b) sticky global | (c) por device (IP/MAC) |
|---|---|---|---|
| Devices afetados | exatamente 1 | **todos** que transmitirem enquanto ligado | 1, escolhido |
| Risco de OTA acidental | baixo — auto-limpa | **alto**: esquecer de desmarcar derruba a planta inteira em AP | baixo |
| Estado no servidor | 1 bool | 1 bool | dict device→bool + expiração |
| Precisa de identidade do device | não | não | **sim** (não existe hoje) |
| Falha de entrega | recuperável (re-arma) | irrelevante | recuperável |
| Frota com N devices | não escolhe **qual** | atualiza todos (bom p/ rollout, péssimo p/ acidente) | escolhe |

Concordo com (a): é a única que não depende de identidade de device e a única segura contra esquecimento. **(b) é ativamente perigosa** com `sleep_min=240`: um flag esquecido continua derrubando devices em AP por horas, e cada um só volta ao normal após 5 min de timeout.

### 3.3 Problema de identidade do device

Hoje o device é identificado **só pelo IP** (`ConnectionEntry.ip`, `tcp_server.py:133`) e o IP vem de DHCP. Não há MAC, serial nem device-id no protocolo. Consequências práticas:

- Com vários sensores na correia, **quem transmitir primeiro leva o OTA**. O operador não escolhe.
- O operador só descobre **qual** device foi para OTA quando lê o SSID `"Update driver - <MAC>"` — ou seja, depois do fato.
- Mitigação sem mexer no fio: a UI mostra, no histórico, o **IP + horário** da conexão que recebeu `ota_sent=True`. Isso responde "qual device foi" para quem tenha o mapa IP→ativo.
- Mitigação real: acrescentar MAC (6 B) ou device-id (2 B) ao **uplink**. Isso é **mudança de contrato de fio** e precisa do lugar certo (provavelmente antes do header, ou header estendido) + mirror em C. **Fora do escopo desta rodada — proponho como fase 2, decisão do lead/bigboss.**

### 3.4 O que o operador precisa fazer fisicamente (transporte SoftAP — D4)

Sequência completa, para virar procedimento em `docs/backend/`:

1. Na rede da planta, abrir `http://<servidor>:8080`, marcar **"OTA na próxima transmissão"**, SALVAR. Badge fica `ARMADO`.
2. **Forçar uma transmissão**: passar o ímã no sensor alvo. Sem isso a espera pode chegar a `sleep_min = 240 min`. Este passo é o que dá algum controle sobre *qual* device pega o flag (§3.3).
3. O device transmite, recebe `update=1`, grava em Preferences, **reinicia** e sobe como Access Point `Update driver - <MAC>`, senha `12345678`.
4. O operador **sai da WiFi da planta** e conecta o notebook/celular nesse AP (o servidor fica inacessível enquanto isso).
5. Abre `http://192.168.4.1/` (porta 80, `updatePage` do `webpage.h`), envia o `.bin` em `/update`. `/version` confere a versão; `/restartESP` reinicia.
6. **Janela de 5 minutos.** Estourou o timeout → o device reinicia sozinho no modo normal, **e o flag já foi consumido** (o servidor limpou no passo 3 e o firmware limpou no boot). É preciso re-armar na UI e repetir do passo 1 — potencialmente mais um ciclo de espera.
7. Voltar o notebook para a WiFi da planta e conferir no histórico que o device voltou a transmitir.

Riscos operacionais a documentar: passo 6 (janela curta + custo alto de repetir), passo 4 (perde acesso ao servidor no meio do procedimento), e o fato de que num ambiente com vários sensores o passo 2 é a única forma de mirar.

### 3.5 OTA em modo STA (device fica na WiFi da planta) — vale propor?

**Vale propor, não vale implementar agora.** D4 é explícita e o SoftAP já existe e funciona no firmware original — reproduzi-lo é custo quase zero. Registro a alternativa para fase 2:

Desenho: no lugar de `update`, o servidor sinaliza "atualize-se"; o device, ainda em STA, faz `HTTPUpdate` contra `http://<servidor>:8080/firmware/latest.bin`, com `/firmware/version` + MD5 para checagem.

| | SoftAP (D4, atual) | STA / pull do backend |
|---|---|---|
| Deslocamento do operador | vai até o sensor, troca de rede | **nenhum** — arma e acompanha pela UI |
| Janela de tempo | 5 min, dura | ilimitada — o device baixa sozinho |
| Frota | 1 por vez, manual | rollout em massa possível |
| Acesso ao servidor durante o processo | perdido | mantido |
| Risco de brick | **baixo** — o operador está fisicamente do lado | **maior** — imagem ruim derruba o device sem acesso físico; exige rollback/2 slots de OTA |
| Trabalho no firmware | zero (já existe) | `HTTPUpdate`, verificação de MD5, tratamento de erro, revisão do particionamento OTA |
| Trabalho no backend | zero | endpoint de firmware + versão + MD5 no Flask (barato, o Flask já roda) |
| Contrato de fio | nenhum novo | precisa de versão/URL — mais campos ou HTTP à parte |

Recomendação: manter D4 agora. Se a manutenção de campo doer (é o cenário provável: 240 min de sleep + trocar de rede + 5 min de janela), a fase 2 STA é barata do lado do backend e o custo real está no firmware e no esquema de partições — decisão de gomi/bigboss.

---

## 4. Lista de testes que falham primeiro (TDD)

Ordem de implementação: protocolo → estado → TCP → web. Cada bloco: escrever, ver falhar, implementar o mínimo.

### 4.1 `backend/tests/test_packet.py` (arquivo NOVO)

| Teste | Asserção |
|---|---|
| `test_server_config_size_is_10` | `SERVER_CONFIG_SIZE == 10` |
| `test_pack_server_config_returns_10_bytes` | `len(pack_server_config(240,20,5,5,0)) == 10` |
| `test_pack_server_config_field_order` | `struct.unpack('<HHHHH', pack_server_config(240,20,5,5,1)) == (240,20,5,5,1)` |
| `test_pack_server_config_matches_win_server_bytes` | `pack_server_config(240,20,5,5,1) == b'\xf0\x00\x14\x00\x05\x00\x05\x00\x01\x00'` (golden do `win_server.py:114`) |
| `test_pack_server_config_rejects_update_out_of_range` | `pytest.raises(ValueError)` para `update=2` |
| `test_pack_server_config_requires_update_argument` | `pytest.raises(TypeError)` chamando com 4 argumentos |
| `test_parse_sample_ignores_trailing_bytes` | `parse_sample` de 18 B devolve 8 campos (regressão, já passa) |

### 4.2 `backend/tests/test_app_state.py` (acrescentar)

| Teste | Asserção |
|---|---|
| `test_ota_armed_defaults_false` | `AppState().ota_armed is False` |
| `test_take_config_for_send_returns_false_when_disarmed` | `_, ota = state.take_config_for_send(); ota is False` |
| `test_take_config_for_send_claims_once_then_clears` | armar; 1ª chamada `True`; 2ª `False`; `state.ota_armed is False` |
| `test_take_config_for_send_returns_config_snapshot` | mutar `state.config` depois da chamada não altera a cópia retornada |
| `test_take_config_for_send_is_atomic_under_concurrency` | armar; 20 threads chamam; **exatamente 1** recebe `True` |
| `test_rearm_ota_sets_flag_back` | claim → `rearm_ota()` → próximo claim devolve `True` |
| `test_connection_entry_ota_sent_defaults_false` | `ConnectionEntry(...).ota_sent is False` |

### 4.3 `backend/tests/test_tcp_server.py` (acrescentar / atualizar)

Atualizar: `test_handle_client_sends_config_from_state` passa a desempacotar `'<HHHHH'` (falha hoje: `struct.error: unpack requires a buffer of 8 bytes`).

**Protocolo/OTA**

| Teste | Asserção |
|---|---|
| `test_handle_client_sends_10_byte_response` | `len(conn.sendall.call_args[0][0]) == 10` |
| `test_handle_client_sends_update_zero_when_disarmed` | 5º campo `== 0` |
| `test_handle_client_sends_update_one_when_armed` | armar; 5º campo `== 1` |
| `test_handle_client_clears_ota_after_send` | após 1 conexão armada, `state.ota_armed is False` |
| `test_second_client_gets_update_zero` | duas conexões seguidas; 1ª `update=1`, 2ª `update=0` |
| `test_handle_client_logs_ota_sent_in_entry` | `state.connections[0].ota_sent is True` na conexão armada |
| `test_handle_client_rearms_ota_when_send_fails` | `conn.sendall.side_effect = OSError`; `state.ota_armed is True` de novo |
| `test_concurrent_clients_only_one_gets_update` | 5 conexões em paralelo com o flag armado; exatamente 1 recebe `update=1` |

**Bugs do item 1**

| Teste | Bug | Asserção |
|---|---|---|
| `test_handle_client_reads_battery_when_payload_not_frame_aligned` | B1 | payload de `18*3 + 16` B + bateria `3800`: `entry.battery_mv == 3800`, `entry.n_samples == 3`, config enviada |
| `test_handle_client_discards_trailing_partial_frame` | B1 | mesmo cenário: **3** amostras, não 4 |
| `test_handle_client_handles_split_header` | B2 | `recv` devolve 2 B + 2 B do header: `expected` correto, config enviada |
| `test_handle_client_does_not_loop_forever_when_peer_closes_before_battery` | B3 | `recv` devolve `b''` no ponto da bateria: `handle_client` **retorna** (teste com `pytest.mark.timeout` ou `side_effect` finito que levantaria `StopIteration` se o loop insistisse), `battery_mv == -1`, config **ainda** enviada |
| `test_handle_client_sends_config_even_when_battery_missing` | B3 | como no original: `sendall` chamado uma vez |
| `test_handle_client_csv_rows_have_battery_column_on_truncated_transfer` | B4 | payload truncado: toda linha enfileirada tem `len(SAMPLE_COLUMNS) + 1` campos |
| `test_handle_client_rejects_absurd_expected_size` | B8 | header `0xFFFFFFFF`: retorna sem acumular, sem `sendall`, sem entrada no histórico |
| `test_handle_client_logs_connection_even_when_send_fails` | B7 | `sendall` levanta `OSError`: existe `ConnectionEntry` com `ota_sent False` |

### 4.4 `backend/tests/test_web_server.py` (acrescentar)

| Teste | Asserção |
|---|---|
| `test_get_index_shows_ota_checkbox` | `'name="ota_armed"'` no corpo; **sem** `checked` por default |
| `test_post_config_arms_ota_when_checkbox_present` | POST com `ota_armed='on'` → `state.ota_armed is True`, 303 |
| `test_post_config_disarms_ota_when_checkbox_absent` | armar antes; POST sem o campo → `state.ota_armed is False` |
| `test_post_config_invalid_input_does_not_change_ota` | armar; POST com `sleep_min='abc'` → 400 **e** `state.ota_armed is True` |
| `test_get_index_shows_armed_badge` | `state.ota_armed = True` → `'ARMADO'` no corpo |
| `test_get_index_shows_ota_column_in_history` | entrada com `ota_sent=True` → `'<td>SIM</td>'` no corpo |
| `test_get_index_history_empty_row_spans_five_columns` | `'colspan="5"'` |

### 4.5 Pendente de decisão (não escrevo sem resposta — §6 Q1)

| Teste | Asserção |
|---|---|
| `test_sample_columns_match_original_csv_header` | `SAMPLE_COLUMNS == ['timestamp','x_data','x_gyro','y_data','y_gyro','z_data','z_gyro','temp']` e a coluna de bateria é `battery_voltage` |

Total estimado: **16 atuais + ~38 novos**.

---

## 5. Ordem de implementação sugerida

1. `test_packet.py` + `packet.py` (10 B, ordem dos campos) — muda o contrato de fio, é o que gomi precisa espelhar em C **primeiro**.
2. `AppState.take_config_for_send` / `rearm_ota` / `ota_sent`.
3. `handle_client`: envio do `update` + `rearm` na falha + `ConnectionEntry.ota_sent`.
4. Correções B1/B2/B3/B4/B8 via `recv_exact` (bloco isolado, testável sem tocar no OTA).
5. Flask + template (checkbox, badge, coluna OTA).
6. B5/B6/B7/B9 (limpeza), conforme a resposta da Q1.
7. README do backend: seção "OTA" + procedimento de campo do §3.4.

## 6. Perguntas ao lead antes de escrever código

> **Resolvidas na rodada 2 (ver §7):** Q1 → DEC-3 (reverter para o header original),
> Q2 → DEC-4 (OTA + B1,B2,B3,B4,B8 + B6,B7,B9 na mesma rodada),
> Q3 → DEC-5 (rota `POST /ota` separada), Q4 → DEC-6 (device-id fica para a fase 2),
> Q5 → DEC-7 (o lead roda os comandos).

- **Q1 — nomes das colunas do CSV (B5).** Volto para os nomes originais (`x_data`, `x_gyro`, ..., `battery_voltage`), que é o que `backend/tools/analysis/cliente_local_csv.py:107-110` e todo o corpus histórico usam, ou mantenho `accel_x/gyro_x/battery_mv` e **eu** atualizo o script de análise? Decisão de produto — não decido sozinho.
- **Q2 — escopo desta rodada.** Faço só o OTA (itens 1–3, 5 acima) ou já entram as correções de robustez B1–B4/B8 no mesmo lote? B1 é acionado deterministicamente pelo wrap do buffer de 700000 B (D2), então na minha leitura ele **precisa** entrar junto.
- **Q3 — desarmar OTA junto com SALVAR.** O checkbox está no mesmo formulário da config: salvar a config com a caixa desmarcada **desarma** o OTA. Aceito, ou você prefere um botão/rota separada (`POST /ota`) para armar/desarmar sem tocar nos 4 parâmetros?
- **Q4 — identidade do device (§3.3).** Confirmo que MAC/device-id no uplink fica **fora** desta rodada? É mudança de contrato de fio e teria de ser espelhada em C.
- **Q5 — pytest.** Não consigo rodar a suíte (shell read-only, venv em `backend/.venv`). Preciso que você rode `cd backend && ./.venv/bin/python -m pytest tests/ -q` a cada etapa e me devolva a saída. Confirma?

---

## 7. Limitações conhecidas — registradas, não corrigidas nesta rodada

### L1 — O timeout do socket é por `recv`, não acumulado na conexão

`conn.settimeout(CLIENT_TIMEOUT_SEC)` (`backend/server/tcp_server.py:88`) arma um timeout de 6 s **por chamada de `recv`**. Com `recv_exact` em laço, um peer que entrega 1 byte a cada 5,9 s **nunca** estoura o timeout e segura um worker do pool pelo tempo que quiser. Dez peers assim esgotam `max_workers=10` e o servidor para de processar conexões novas.

O que **não** é: não é o B3. `recv_exact` garante progresso — leitura de comprimento zero levanta `ConnectionError` na hora, então não existe mais o laço que gira a 100 % de CPU sem receber nada. L1 exige um device hostil ou gravemente defeituoso na LAN da planta, entregando bytes de verdade, devagar.

Por que fica de fora agora: **o servidor de produção tem exatamente a mesma exposição** (`pyFiles/win_server.py:60` + laço de `recv`), então corrigir isso é mudança de comportamento nova, não correção de regressão — DEC-0 manda deixar quieto. Perfil de risco diferente do conserto de framing e não deve pegar carona nele.

Mitigações possíveis quando for a hora (uma OU outra, não as duas):
- **Deadline acumulado por conexão**: marca `t0` no `accept`, e cada `recv_exact` recalcula `conn.settimeout(deadline - now)`, abortando quando o orçamento total acaba. Simples, mas precisa de um orçamento generoso o bastante para o pior caso real de 699984 B em WiFi de planta.
- **Piso de throughput**: mede bytes/s ao longo da conexão e derruba abaixo de um mínimo. Mais tolerante a uma transferência legitimamente lenta, mais código e mais um número para calibrar.

Preferência, se/quando entrar: deadline acumulado, dimensionado a partir do throughput medido em campo pelo firmware (o original já imprime kbps ao fim de cada transmissão), com folga larga.

#### L1b — O mesmo timeout limita o desligamento

O laço de recepção era gateado no global `running` (`while running: conn.recv(...)`), então um desligamento no meio de uma transferência abandonava a conexão. Com a leitura por `recv_exact` não existe mais laço nem gate: uma transferência em curso vai até o fim ou até o timeout do socket. `server_main` continua checando `running` no laço de `accept`, então o desligamento funciona — só não é mais instantâneo para uma conexão já em andamento.

Isso é deliberado, não esquecimento. Não havia comportamento bem definido a preservar: a versão de "abandonar" do servidor de produção era estourar `UnboundLocalError` no `finally` com `battery_voltage` não-vinculado e matar o worker. Restaurar o gate significaria **projetar** o que é um abandono limpo — decisão nova, sem teste por trás.

Consequência que liga L1b a L1: o mesmo peer lento que segura um worker também **atrasa o desligamento**, porque o dreno das conexões em curso é limitado pelo mesmo timeout por `recv`. É a mesma propriedade vista de outro ângulo — a mitigação de L1 (deadline acumulado) fecha as duas.
