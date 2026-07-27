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

    def set_ota_armed(self, armed: bool) -> None:
        """Arma ou desarma o flag one-shot (rota POST /ota)."""
        with self.lock:
            self.ota_armed = bool(armed)

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

`set_ota_armed` existe para que a rota Flask não escreva `state.lock` à mão: toda mutação do armamento passa por um único método.

### 2.5 `backend/server/tcp_server.py`

Implementado assim (`tcp_server.py:170-198`):

```python
config, ota = state.take_config_for_send()
response = pack_server_config(
    config.sleep_min, config.idle_min,
    config.max_acq,   config.cooldown_sec, int(ota)
)

try:
    conn.sendall(response)
    logging.info(f'Config sent to {addr[0]} (update={int(ota)})')
except OSError as e:
    if ota:
        state.rearm_ota()   # ninguém recebeu — devolve o armamento
        ota = False         # e a entrada do histórico diz a verdade
    logging.error(f'Config not delivered to {addr[0]}: {e}')

with state.lock:
    state.connections.append(ConnectionEntry(..., ota_sent=ota))
```

O `OSError` é **tratado ali, não relançado** — de propósito. Relançar pularia o registro no histórico, e a correção de **B7** é justamente registrar a conexão também no caminho de erro: as amostras chegaram. `ota` é forçado a `False` nesse caminho porque o flag não chegou ao device; um histórico que afirmasse o contrário mandaria o operador procurar um AP que nunca vai aparecer.

Consequência de hierarquia de exceções, deliberada: `socket.timeout` **é** `TimeoutError`, que **é** subclasse de `OSError`, então um `sendall` que expira cai neste `except` local em vez do `except socket.timeout` externo. É o comportamento desejado — envio que expirou é envio que não chegou, logo o armamento volta e a conexão continua registrada.

O docstring do módulo (`tcp_server.py:11-14`) passou de `[8 bytes]`/4 campos para `[10 bytes]`/5 campos.

### 2.6 `backend/web/server.py` — rota separada (DEC-5)

O formulário de config **não** lê nem escreve o armamento. Rota própria:

```python
@app.route('/ota', methods=['POST'])
def update_ota():
    armed = request.form.get('armed')
    if armed not in ('0', '1'):
        reason = ('armed: campo ausente.' if armed is None
                  else f'armed: "{armed}" nao e um valor valido.')
        return Response(f'{reason} Use 1 para armar o OTA ou 0 para desarmar.',
                        status=400, mimetype='text/plain')
    state.set_ota_armed(armed == '1')
    return redirect('/', 303)
```

`GET /` lê `ota_armed` **dentro do mesmo lock** da cópia da config e passa ao template, e responde com `Cache-Control: no-store`: a página é o único instrumento do operador, e um render de cache mostrando armamento velho é pior que página nenhuma.

Validação do formulário de config (`validate_config_form`), rescrita na mesma rodada:
- **todos** os campos inválidos são reportados de uma vez, um por linha, cada um nomeando o campo e o motivo — o operador está de pé com um laptop na planta, e uma segunda rejeição depois de corrigir a primeira parece formulário quebrado;
- ordem fixa, vinda de `FORM_FIELDS`, nunca de iteração de dict;
- nada é escrito quando algo está errado — config aplicada pela metade é config que operador nenhum escolheu, exibida como se alguém tivesse querido;
- limite superior `UINT16_MAX` (65535), que mora em `protocol/packet.py` porque é a **largura do campo no fio**, não política. Sem ele o formulário aceitava `sleep_min=999999`, mostrava salvo, e toda conexão de device morria dentro de `pack_server_config` num `struct.error` engolido pelo `except` largo.

### 2.7 `backend/web/templates/index.html`

**Dois botões, não checkbox.** Checkbox só teria efeito no segundo clique do SALVAR, e entre marcar e salvar a página mostraria armado com nada armado — exatamente a ambiguidade que se quer eliminar. O botão **é** o ato.

- Painel esquerdo, bloco "Atualizacao OTA" abaixo do formulário:
  - desarmado: faixa `OTA desarmado` + botão **ARMAR OTA** (`POST /ota`, `armed=1`);
  - armado: faixa `OTA ARMADO — proximo device que transmitir` em `#e94560` + botão **DESARMAR** (`armed=0`, estilo secundário, para armar e desarmar não parecerem o mesmo ato).
- Histórico ganha a coluna **OTA**: a linha que levou o flag recebe `class="ota-row"` (fundo tingido, borda esquerda `#e94560`) **e** um `<span class="ota-badge">OTA</span>`. Cor, posição **e** texto — cor sozinha falha para operador daltônico e em impressão preto e branco, que acontece em planta.
- `colspan` da linha vazia: 4 → 5.

### 2.9 Matriz de compatibilidade firmware × servidor

| Firmware | Servidor | Resultado |
|---|---|---|
| antigo (lê 8 B) | backend novo (manda 10 B) | **OK, degrada limpo.** Lê os 8 primeiros bytes — mesmos 4 campos, mesma ordem —, deixa 2 não lidos num socket que vai fechar, e nunca entra em OTA. |
| novo (exige 10 B) | `pyFiles/win_server.py` | **OK, byte a byte.** Empacota `'<HHHHH'` com os mesmos campos, OTA inclusive. |
| novo | backend novo | **OK.** Contrato desta rodada. |
| novo | `pyFiles/server_lix_csv2.py` | **QUEBRADO.** Esse servidor responde 8 bytes; o firmware espera 10, desiste após 5 s e fica **sem config nenhuma**, mantendo os defaults em silêncio. Device parece saudável e ignora toda mudança de configuração para sempre. |

### 2.10 Cláusulas de contrato — assinadas em 2026-07-27

Saíram da leitura conjunta entre as duas metades. Nenhuma muda o layout: continuam 10 bytes, `update` no offset 8. Duas mudam **o que um valor significa**, e uma registra uma propriedade que emergiu das duas metades juntas.

#### Cláusula 1 — `update = 0` é uma instrução de **desarme** no device

Ao receber uma config com `update == 0`, se o flag persistido estiver ligado, o device o **apaga**. Lê antes de escrever: uma gravação em NVS na transição, nenhuma em regime.

Isto é **desvio deliberado do original**, onde `main.cpp:282` age em `if (response.update)` sem `else` e o 0 é silêncio. A justificativa não é "é melhor": é que já aceitamos um desvio que **cria** o problema que este resolve. Manter o flag ligado quando o Access Point falha a subir é escolha nossa — o original sempre limpa no boot e por isso não tem caso de flag latente. Tendo criado a trava, devemos a ela um limite.

Limite melhor que o contador descartado: contador para de tentar e deixa um device que nunca atualiza; o desarme devolve o device a um estado conhecido na próxima transmissão comum.

**O backend não muda nada.** Já envia `update=0` em toda conexão que não vence o claim.

> **Isto não é auto-cura, e a frase importa.** A intenção do operador é **descartada**, não atendida. "Qualquer transmissão normal limpa um flag preso" soa como pedido honrado — não é: é pedido **perdido**, só que perdido melhor. Perdido pronta e visivelmente, com o operador ainda na mesa, em vez de latente e horas depois. Ninguém deve ler o desarme como garantia de entrega.

#### Cláusula 2 — o device **não pode transmitir** entre receber `update=1` e entrar no boot de OTA

Vira **requisito** do contrato, e não mais efeito colateral aceito da D3.

Sob a cláusula 1, uma transmissão nesse intervalo colhe `update=0` e **desarma o flag que o device acabou de receber**. O OTA nunca aconteceria, e o sintoma apareceria no backend parecendo bug de servidor.

Hoje vale porque o original reinicia imediatamente após persistir (`main.cpp:284-288`). O abandono das aquisições restantes daquele wake **era** efeito colateral e **passou a ser** requisito — e essa mudança de status é o ponto todo: efeito colateral pode ser otimizado por quem nunca soube que ele sustentava algo. Um refactor "termina o ciclo antes de reiniciar, fica mais limpo" quebra isto em silêncio. Defesa: `T21b` (`test_a_pending_update_restarts_before_any_further_acquisition`), na precedência do `belt_next` — a adjacência persist/restart é estrutural e não testável, a precedência é testável e é a metade que um refactor reescreve.

#### Cláusula 3 — o cliente aplica a config **tudo-ou-nada**

> Um frame menor que `SERVER_CONFIG_WIRE_BYTES` não altera **campo nenhum**.

`packet.cpp` rejeita por comprimento **antes** de ler qualquer campo, e `update` só é consultado quando o parse teve sucesso. Fixado por `test_config_is_applied_only_when_ten_bytes_arrive` (T46), que roteia uma resposta de 9 bytes e exige os cinco campos inalterados.

**Correção registrada:** esta cláusula foi assinada primeiro com a justificativa errada — a de que a proteção vinha de `update` ser o **último** campo, e portanto de o offset 8 ser intocável. Não é. A proteção é o parse tudo-ou-nada; mover o campo para o offset 0 não muda nada, porque um prefixo de 2 bytes com `update=1` é rejeitado inteiro. A versão errada era pior que errada de duas formas ao mesmo tempo: restringia o layout à toa **e** deixava a invariante real desprotegida — um cliente futuro que aplicasse campos conforme chegam recriaria o problema em qualquer offset, com todo mundo atrás de uma regra sobre ordem de campo que não protegia nada. `update` fica no offset 8 pelo motivo que sempre teve: compatibilidade de fio com o `win_server.py`. É suficiente e não precisa de restrição adicional.

#### Consequência das três: um armamento nunca fica nos dois lados

Direção do raciocínio, e vale como propriedade de segurança entre as metades:

> device vê `update=1` ⇒ chegou um frame **completo** ⇒ o `sendall` **não** levantou ⇒ o backend **limpou** em vez de re-armar.

Logo **uma conexão nunca deixa o flag no device E de volta no servidor**. Consequência da regra tudo-ou-nada da cláusula 3, não do offset do campo.

Do lado do backend a implicação depende de um fato do `sendall`, confirmado: ele faz laço sobre `send()` e retorna assim que o kernel aceita o resto, sem verificação posterior. Se levanta, ao menos um byte foi recusado; se retorna, os dez foram aceitos. Falha de TCP posterior só apareceria num `send` seguinte, e não existe `send` seguinte — `handle_client` envia uma vez e cai no `conn.close()` do `finally`.

Note o que a propriedade **não** diz: "kernel aceitou dez bytes" não é "device recebeu dez bytes". Por isso ela é *o device não consegue se armar a partir de um envio que o backend considera falho*, e **não** *o device se armou sempre que o backend considera enviado*. Este segundo caso é a terceira saída da L2, e continua aberto.

#### Dependência estreita do firmware — registrada como assimetria

O firmware exige apenas: **uma resposta, se chegar, carrega um `update` verdadeiro.** Não exige que uma resposta sempre chegue.

A garantia do backend é mais forte: `handle_client` responde em toda transmissão completa, e `update` é `int(ota)` vindo do claim. Os caminhos sem resposta são quatro — timeout/`ConnectionError` no header, header acima de `MAX_PAYLOAD_BYTES`, payload truncado, exceção inesperada antes do claim — e em todos **o claim nunca acontece**, então nenhum armamento é gasto e o flag do device fica como estava.

Garantia mais forte que a dependência significa que um enfraquecimento futuro do lado do backend **atrasa** um desarme em vez de corromper um. O que quebraria o firmware é um **quinto caminho que responda sem passar pelo claim** — daí o comentário `CROSS-HALF INVARIANT` no ponto do claim em `handle_client`: o estrago cairia na outra metade, e nenhum teste do backend pegaria.

---

#### Patch sugerido para `server_lix_csv2.py` — para o bigboss aplicar

> **Não aplicado por nós, e não testado por nós.** O arquivo vive em **outro repositório** (`SIF-DI241794-...`, privado) que está **em produção**. Ninguém deste time tocou nele. Não temos como executá-lo aqui: o texto abaixo foi derivado por leitura, comparando-o com `win_server.py`, que já emite os 10 bytes corretos.

Uma linha, em `pyFiles/server_lix_csv2.py:124`.

Antes:
```python
                    interTriggerTime = 5 
                    resposta = struct.pack('<HHHH', sleepTime, untriggerTime, nSamples, interTriggerTime)
```

Depois:
```python
                    interTriggerTime = 5
                    update = 0   # 1 coloca o proximo device em modo OTA
                    resposta = struct.pack('<HHHHH', sleepTime, untriggerTime, nSamples, interTriggerTime, update)
```

`update = 0` como default é deliberado: este servidor não tem interface para armar OTA, e um `1` fixo colocaria **todo** device que transmitisse em modo AP. Quem quiser usar este servidor para atualizar um device troca para `1`, atualiza, e volta para `0` — é o que o `win_server.py` faz hoje, e note que ele está com **`update = 1` fixo** (`win_server.py:113`), o que é uma armadilha própria dele.

**O que verificar depois de aplicar** (quem aplicar não terá a nossa suíte):
1. o device recebe **10 bytes** e não expira a espera — no serial do firmware, a linha de resposta do servidor aparece em vez de "Nenhuma resposta recebida do servidor";
2. os **quatro campos existentes chegam inalterados** — sleep, untrigger, nSamples e interTrigger com os mesmos valores de antes do patch;
3. com `update = 0`, o device **não** reinicia em modo AP depois da transmissão.

### 2.8 Lado firmware (informativo — dono: sifemb-gomi, eu não edito)

Estado verificado em 2026-07-27, lendo `lib/` e `src/` ao lado do backend:

- `lib/protocol/packet.h`: **já feito.** `struct ServerConfig` tem `uint16_t update;` como quinto e último campo, `packed`, com `static_assert(sizeof(ServerConfig) == 10)` e `DEFAULT_UPDATE 0`. Bate byte a byte com `pack_server_config('<HHHHH', ...)`.
- `src/services/connectivity/tcp_client.cpp:31`: a espera `_client.available() < sizeof(ServerConfig)` acompanhou o struct de 8 para 10 sem edição; o timeout de 5000 ms segue válido.
- **Nada em `src/` age sobre `update`** — `grep` por `update`, `Preferences`, `restart`, `softAP`, `WIFI_AP` em `src/`: zero ocorrências. Ver **L2** na seção 7: hoje armar o OTA consome o flag e não acontece nada no device.
- `packet.h` declara `parse_server_config(bytes, len, out)` com contrato explícito (exige os 10 bytes, deixa `out` intacto em frame curto), mas `tcp_client.cpp` não chama essa função — lê direto para dentro do struct com `readBytes`. A checagem de `available()` torna isso seguro hoje, então não é bug vivo, mas o decodificador cuidadoso está sem uso e o caminho vivo é o sem contrato. Observação para o dono de `src/`.
Fluxo OTA do original a reproduzir: `main.cpp:282-289` (grava `Preferences("config").putBool("update", true)` + `ESP.restart()`) e `main.cpp:73-96` (no boot, se o flag está setado: limpa o flag, sobe `WIFI_AP` `"Update driver - <MAC>"` / `12345678`, `configureOtaServer()`, timeout de 5 min → `ESP.restart()`).

---

## 3. UX e semântica do armamento OTA

### 3.1 Decisão vigente: (a) one-shot na próxima conexão — **D3, binding**

O operador clica **ARMAR OTA**; o servidor manda `update=1` para o **próximo** device que completar uma transmissão e limpa o flag na mesma operação atômica. O device seguinte recebe `update=0`.

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

## 4. A suíte como ela existe (110 casos)

Escrita em TDD, um bloco por vez: teste falhando primeiro, mínimo para passar, depois o próximo. Esta seção descreve o que **existe**, não o que se planejou.

Baseline: `cd backend && ./.venv/bin/python -m pytest tests/ -q` → **110 passed**.

### 4.1 `backend/tests/test_packet.py` — 14 casos, contrato de fio

| Teste | Asserção |
|---|---|
| `test_server_config_size_is_10` | `SERVER_CONFIG_SIZE == 10` |
| `test_pack_server_config_returns_10_bytes` | `len(...) == SERVER_CONFIG_SIZE` |
| `test_pack_server_config_field_order` | `struct.unpack('<HHHHH', pack(240,20,5,5,1)) == (240,20,5,5,1)` |
| `test_pack_server_config_matches_win_server_golden_bytes` | `== b'\xf0\x00\x14\x00\x05\x00\x05\x00\x01\x00'`, golden de `win_server.py:114` |
| `test_pack_server_config_defaults_match_settings` | defaults + `update=0` reproduzem o golden |
| `test_pack_server_config_requires_update_argument` | `TypeError` com 4 argumentos |
| `test_pack_server_config_rejects_update_out_of_range[2, 65535, -1]` | `ValueError` (3 casos) |
| `test_csv_columns_match_original_header` | `CSV_COLUMNS` == literal de 9 strings do servidor de produção |
| `test_sample_columns_are_the_header_without_the_battery` | `CSV_COLUMNS == SAMPLE_COLUMNS + ['battery_voltage']` |
| `test_frame_size_constants` | 18 / 4 / 2 |
| `test_parse_sample_field_order_and_signedness` | `'<I7h'` ida e volta, com negativos |
| `test_parse_sample_returns_one_field_per_sample_column` | 8 campos |

O header do CSV é comparado contra um **literal**, não contra `CSV_COLUMNS`: comparar com a constante só provaria que o writer concorda com ela — que era exatamente o caso quando ambos estavam errados.

### 4.1b `backend/tests/test_recv_exact.py` — 9 casos, framing

`returns_exactly_n_bytes` | `uses_one_recv_when_everything_arrives_at_once` | `concatenates_one_byte_chunks` | **`requests_only_the_missing_bytes`** | `raises_connection_error_when_peer_closes` | `does_not_spin_when_peer_closes` | `error_reports_partial_progress` (`'3/8'`) | `zero_bytes_returns_empty_without_calling_recv` | `propagates_socket_timeout`

O stub de peer fechado **conta chamadas** e levanta `AssertionError` depois de 10, em vez de devolver `b''` para sempre: teste que pode travar acaba travando a CI de alguém às 3 da manhã. `requests_only_the_missing_bytes` está em negrito de propósito — ver **L6** na seção 7.

### 4.2 `backend/tests/test_app_state.py` — 6 antigos + 12 do armamento

| Teste | Asserção |
|---|---|
| `test_ota_armed_defaults_false` | `AppState().ota_armed is False` |
| `test_ota_armed_default_matches_settings` | `is bool(DEFAULT_UPDATE)` |
| `test_set_ota_armed_arms_and_disarms` | ida e volta |
| `test_take_config_for_send_returns_false_when_disarmed` | `ota is False` |
| `test_take_config_for_send_returns_current_config_values` | devolve os 4 valores correntes, tipo `DeviceConfig` |
| `test_take_config_for_send_returns_config_snapshot` | mutar `state.config` depois não altera a cópia devolvida |
| `test_take_config_for_send_claims_once_then_clears` | 1ª `True`, 2ª `False`, `ota_armed is False` |
| `test_take_config_for_send_is_atomic_under_concurrency` | `threading.Barrier(20)`; **exatamente 1** recebe `True`; erros de worker coletados e conferidos vazios |
| `test_rearm_ota_gives_arming_back_after_failed_send` | claim → `rearm_ota()` → próximo claim `True` |
| `test_rearm_ota_twice_does_not_double_arm` | dois rearms → um único claim `True` (booleano, não contador) |
| `test_connection_entry_ota_sent_defaults_false` | default `False` |
| `test_connection_entry_records_ota_sent` | aceita `ota_sent=True` |

O teste de concorrência usa `Barrier` para as 20 threads reivindicarem no mesmo instante — sem ele o "teste de corrida" passa por sorte de escalonamento. `test_connections_max_50` (antigo) fixa o `maxlen=50`, então mexer nesse número exige tocar num teste: ver **L3**.

### 4.3 `backend/tests/test_tcp_server.py` — 3 antigos (1 atualizado) + 25

`test_handle_client_sends_config_from_state` foi **atualizado** para desempacotar `'<HHHHH'` e conferir o 5º campo. Ele estava vermelho por afirmar o contrato de 8 bytes; corrigir a asserção foi a correção inteira — nenhuma mudança de produção mereceu crédito por isso.

**Framing (B1/B2/B8)**

| Teste | Asserção |
|---|---|
| `test_handle_client_parses_every_complete_frame` | 3 frames → `n_samples == 3` |
| `test_handle_client_reads_battery_when_payload_is_not_frame_aligned` | `18*3 + 16` B: `battery_mv == 3800` (não engolido pelo parser) |
| `test_handle_client_discards_trailing_partial_frame_legacy_firmware_compat` | mesmo cenário: **3** amostras, não 4 |
| `test_handle_client_reassembles_split_header` | header em 1 B + 3 B |
| `test_handle_client_reassembles_chunked_payload` | corpo em 3 pedaços |
| `test_handle_client_handles_empty_payload` | header 0: config enviada, `n_samples == 0`, `recv_exact(0)` não toca no socket |
| `test_handle_client_rejects_absurd_expected_size` | `0xFFFFFFFF`: `recv.call_count == 1`, sem `sendall`, sem histórico |
| `test_handle_client_rejects_payload_over_max` | `MAX_PAYLOAD_BYTES + 1`: idem |

As duas rejeições afirmam `recv.call_count == 1` — sem isso passariam por "estourou durante a leitura" em vez de "recusou antes de alocar".

**Bateria (B3, DEC-4)** — `sends_config_when_battery_is_missing` | `logs_invalid_battery_when_battery_is_missing` | `sends_config_when_battery_times_out`. Todos fixam a degradação de produção: bateria `-1` **e** `sendall` chamado uma vez.

**Linhas do CSV (B4)** — `queues_rows_with_the_battery_column` | `queues_rows_with_invalid_battery_when_battery_is_missing` | `queues_nothing_when_payload_is_truncated`. Fixture `autouse` drena a `data_queue` global em volta de cada teste, e `queued_rows()` lê o que o `save_data` receberia de fato.

**Saída CSV (B5/DEC-3, B6)** — `save_data_writes_the_original_csv_header` | `save_data_writes_the_battery_in_the_last_column` | `save_data_keeps_the_csv_when_the_drive_copy_fails[2]` | `save_data_does_not_report_a_save_failure_when_only_the_copy_failed[2]` | `save_data_reports_the_drive_copy_failure[2]`. Rodam o `save_data` de verdade (fila + sentinela + leitura do arquivo), com `chdir(tmp_path)` e `subprocess.run` monkeypatchado — nunca chamam `gio`. Os parametrizados cobrem `CalledProcessError` (gvfs ausente) e `FileNotFoundError` (gio não instalado).
`writes_the_battery_in_the_last_column` existe para o verde não poder ser obtido **encurtando** o header em vez de renomear a coluna.

**OTA no fio (B0/D3, B7)**

| Teste | Asserção |
|---|---|
| `test_handle_client_sends_update_zero_when_disarmed` / `..._one_when_armed` | 5º campo 0 / 1 |
| `test_handle_client_clears_ota_after_send` | `ota_armed is False` depois |
| `test_handle_client_second_connection_gets_update_zero` | sequencial: 1ª `1`, 2ª `0` |
| `test_handle_client_logs_ota_sent_in_entry` / `..._not_sent_when_disarmed` | `ota_sent` no histórico |
| `test_handle_client_sends_the_claimed_snapshot_not_a_later_config` | `take_config_for_send` embrulhado num mock que muda a config logo após o snapshot: `call_count == 1` **e** o fio carrega o valor antigo |
| `test_handle_client_rearms_ota_when_send_fails` | `OSError` no `sendall`: flag volta **e** a conexão seguinte recebe `update=1` |
| `test_handle_client_does_not_rearm_when_send_succeeds` | espião em `rearm_ota`: `call_count == 0` |
| `test_handle_client_logs_entry_when_send_fails` | entrada registrada mesmo com envio falho |
| `test_handle_client_logs_ota_not_sent_when_send_fails` | `ota_sent is False` nessa entrada |

`second_connection_gets_update_zero` é **sequencial** de propósito: a propriedade aqui é "o segundo chamador vê o flag limpo". Que dois claims simultâneos não possam ambos vencer é outra propriedade, mora no `test_app_state`, e lá o `Barrier` torna a sobreposição real.
`rearms_ota_when_send_fails` não se contenta com `ota_armed is True` — essa asserção passaria trivialmente contra código que nunca reivindica o flag; por isso roda uma segunda conexão e exige que ela receba `update=1`, que é o que o operador de fato experimenta.

### 4.4 `backend/tests/test_web_server.py` — 8 antigos + 26

**Validação do formulário (13 casos)** — `rejects_zero_max_acq` | `rejects_zero_in_any_field[4]` | `rejects_value_above_uint16[4]` | `accepts_uint16_maximum` (limite **inclusivo**) | `error_message_names_the_offending_field` | `error_message_mentions_the_protocol_limit` | `error_message_lists_every_offender`.
Cinco dos casos afirmam `config_tuple(state)` idêntico antes e depois da rejeição. É a asserção mais valiosa do bloco e o motivo não é óbvio: um 400 que já tivesse aplicado dois dos quatro campos deixa a planta rodando uma configuração que operador nenhum escolheu, e a página exibe como se alguém tivesse querido. Validar-antes-de-mutar hoje é verdade só pela ordem das linhas — a asserção transforma isso em propriedade.
`error_message_lists_every_offender` fixa a **ordem** (`body.index('sleep_min') < body.index('max_acq')`), não só a presença. Os testes de mensagem também afirmam que campos **válidos não aparecem** — sem isso, uma implementação que despeja os quatro nomes em qualquer erro passaria.

**OTA na interface (13 casos)** — `shows_ota_disarmed_by_default` | `shows_armed_badge_before_any_connection` | `armed_page_differs_from_disarmed` | `post_ota_arms` | `post_ota_disarms` | `post_ota_rejects_invalid_value` | `post_ota_rejects_missing_value` | `post_ota_does_not_change_config` | `post_config_does_not_change_ota` | `post_ota_on_a_fresh_server_renders_with_empty_history` | `history_marks_the_entry_that_took_the_ota` | `history_does_not_mark_a_normal_entry` | `history_empty_row_spans_every_column`.

- `armed_page_differs_from_disarmed` renderiza as duas e afirma que os corpos **diferem**: é a exigência "armado não pode parecer desarmado" como propriedade, não como busca de string, então continua valendo quando o markup mudar.
- `post_ota_does_not_change_config` afirma **303** além da config intocada — sem o status, passaria no 404 de uma rota inexistente.
- Os testes de histórico ancoram em `<tr class="ota-row">` e `class="ota-badge"` (forma de atributo), **não** na substring nua do nome da classe: a folha de estilo define `.ota-row` em toda página, armada ou não, então a forma nua passaria a valer sempre. Ancorar na linha renderizada é estritamente mais forte.
- `'>OTA<'` em vez de `'OTA'`: a segunda casaria também com o botão `ARMAR OTA` e passaria numa página sem coluna de histórico nenhuma.

**Total: 110 casos.**

---

## 5. Ordem em que foi implementado

Cada passo: teste vermelho, saída bruta conferida pelo lead, mínimo para o verde, commit. Um commit por passo, para o `git log` contar a história e não o resultado.

| Passo | O que entrou | Commit |
|---|---|---|
| 1 | `test_packet.py` + `packet.py`/`settings.py` — 10 B, ordem dos campos, header CSV revertido (DEC-3) | `041880c` |
| 2 | `AppState`: `ota_armed`, `set_ota_armed`, `take_config_for_send`, `rearm_ota`, `ConnectionEntry.ota_sent` | `35fbca1` |
| S1 | `recv_exact` sozinho — B3 (o laço que girava em `b''`) em commit próprio | `f6af0a3` |
| S2 | `handle_client` lendo por quantidades exatas — B1, B2, B4, B8 | `670112e` |
| S2b | `writer.writerow(CSV_COLUMNS)` — regressão do header CSV introduzida no passo 1 | `3b97f13` |
| S3 | OTA no fio: claim, `rearm` no envio falho, `ota_sent`, B7 | `c20549c` |
| S4a | B6 — falha de cópia para o Drive deixa de ser reportada como perda de dados | `15f0e1b` |
| S4b | Validação por campo + limite `UINT16_MAX` | `066f76f` |
| B9 | Imports e constantes mortos (`BUFFER_SIZE`, `RESPONSE_TIMEOUT_SEC`, dois imports) | `42365f1` |
| S5 | `POST /ota`, botões, badge, coluna OTA no histórico | `76f83fe` |

Pendente: seção "OTA" no `backend/README.md` com o procedimento de campo do §3.4.

### 5.1 Mutação de verificação

Depois do S5, com a suíte em 110 verdes, apagou-se a linha `self.ota_armed = False` de `take_config_for_send` — o claim para de limpar e o one-shot vira **sticky global** em silêncio, que é o que a D3 proíbe e o que colocaria todos os devices da correia em modo AP, um atrás do outro.

Cinco algozes nomeados **antes** de rodar: `take_config_for_send_claims_once_then_clears`, `take_config_for_send_is_atomic_under_concurrency`, `rearm_ota_twice_does_not_double_arm`, `handle_client_clears_ota_after_send`, `handle_client_second_connection_gets_update_zero`.

Resultado: `5 failed, 105 passed` — os cinco, exatamente, em dois módulos e nas duas camadas (a máquina de estados do armamento e a consequência dele no fio). Restaurado e reconferido em 110.

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

### L2 — Nada confirma que o device **agiu** sobre `update=1`

Um `sendall` bem-sucedido prova que 10 bytes chegaram ao **kernel**, e nada além disso. Se o device reiniciar ou falhar antes de o `Preferences.putBool` gravar, o armamento foi **gasto**, o histórico diz `OTA`, e nenhum AP aparece — a mesma falha de "operador caçando um AP fantasma" que o campo `ota_sent` foi criado para evitar, entrando por uma porta que não fechamos.

#### As três saídas de um armamento

Não são duas, são três — e a terceira é a que nenhuma das duas metades enxerga sozinha:

| Saída | Onde fica o armamento | Quem consegue detectar |
|---|---|---|
| `sendall` levanta `OSError` | **de volta no servidor** (`rearm_ota`), entrada de histórico com `ota_sent=False` | backend, e trata |
| device recebe e grava o NVS | **só no device** — o backend já limpou o dele | ninguém confirma, mas o AP aparece |
| `sendall` retorna OK e os bytes **não** chegam (reset, RST, link caindo depois do retorno) | **em lugar nenhum** | **ninguém** |

Na terceira o sistema fica consistente por fora: o servidor limpou porque enviou, o device nunca gravou porque nunca recebeu, e nada está em contradição. O histórico diz `OTA`, nenhum AP sobe, e não existe estado divergente que alguém pudesse inspecionar. É a razão de a ordenação do lado firmware (confirmar o AP antes de limpar o flag) ser a única coisa entre nós e uma perda silenciosa — e é também o motivo de essa ordenação **não** fechar esta saída: o device nunca chegou a ter flag para ordenar.

Só se fecha com **confirmação** — o device dizendo que agiu. Isso é mudança de fio e fica para a fase 2.

**Hoje isso não é hipótese.** Verificado em 2026-07-27: `grep` por `update`, `Preferences`, `restart`, `softAP` e `WIFI_AP` em `src/` não retorna nada. O firmware refatorado recebe o campo e o guarda no struct, mas **nenhum código o lê**. Enquanto o lado embarcado não implementar a ação de OTA, armar pela interface é um no-op que consome o flag em silêncio. Não é defeito do backend — é as duas metades andando em ritmos diferentes —, mas se isso for demonstrado antes de o firmware chegar, vai parecer backend quebrado.

Duas formas candidatas para quando for a hora (nenhuma projetada agora, ambas custam mudança de fio + handshake no firmware):
- **armamento confirmado pelo device**: o device responde algo antes de reiniciar, e só então o servidor limpa;
- **re-armar até ver o device em modo AP**: o flag persiste até uma evidência externa de que o AP subiu.

### L3 — Profundidade do histórico — **DECIDIDO E APLICADO: 500** (bigboss, 2026-07-27)

**Questão levantada:** `AppState.connections` era um `deque(maxlen=50)`, escolhido quando um wake significava **uma** conexão. Pela D1 um wake é até `max_acq=5` aquisições, cada uma com sua conexão TCP e sua linha de histórico. Cinco sensores dão ~25 linhas por rodada, então 50 guardava **duas rodadas**. E a linha que saía primeiro, justamente quando a planta está mais movimentada, era o registro de OTA — o único rastro de qual sensor está prestes a sumir num AP.

**Decisão:** `HISTORY_MAX_CONNECTIONS = 500` (`config/settings.py`), ~20 rodadas. Aplicado no commit `36623bd`. O número que importa não é 500: é **quantas rodadas o operador ainda consegue enxergar a linha de OTA**. Se `max_acq` ou o número de sensores mudar muito, é essa conta que se refaz, não o 500.

Custo: uma `ConnectionEntry` são quatro campos pequenos e um datetime, então 500 delas ficam na casa de algumas centenas de KB, num processo que já mantém 700 KB por conexão em voo. Não é trade-off.

Fixado por dois testes: `test_connections_capped_at_500` (literal, não a constante) e `test_connections_drop_the_oldest_first`. O segundo existe porque nada fixava de **qual ponta** o buffer descarta — e se descartasse pela mais nova, subir o teto de 50 para 500 teria **piorado** o problema, com a suíte inteira verde.

**Questão encerrada.** Não reabrir sem refazer a aritmética acima.

### L4 — `POST /ota` sem autenticação — **RISCO LEVANTADO E ACEITO** (bigboss, 2026-07-27)

Isto não passou despercebido: foi levantado na auditoria do backend, escalado, e **aceito** com a premissa registrada abaixo. Está aqui para que alguém consiga **reabrir** a questão quando a premissa mudar.

**A cadeia, explícita.** O Flask sobe em `0.0.0.0:8080` sem autenticação nenhuma. Qualquer um com acesso de rede faz `POST /ota` com `armed=1`. O próximo device que transmitir reinicia como Access Point **aberto**, SSID `Update driver - <MAC>`, senha fixa `12345678`, servindo uma página de upload de firmware na porta 80 por 5 minutos. Ou seja: de "estar na rede da planta" a "firmware arbitrário rodando num sensor", sem credencial em nenhum ponto.

**Sem cobertura da DEC-0.** A DEC-0 protege comportamento que produção já tinha. Produção **não tinha interface web nenhuma** no servidor — a UI é novidade do refactor, e essa cadeia não existia antes desta rodada. Não dá para chamar isso de fidelidade ao original.

**Premissa da aceitação — é isto que precisa ser reavaliado se mudar:** a rede dos sensores é tratada como **isolada**, sem acesso de terceiros e sem rota para fora. Sob essa premissa o custo de autenticação (gerir credencial num laptop de planta, operador travado do lado de fora no meio de uma manutenção) supera o ganho.

**A premissa deixa de valer se:** a rede da planta passar a ser compartilhada com WiFi corporativo ou de visitantes; o servidor ganhar rota para fora ou VPN; a planta rodar em rede plana com outros equipamentos; ou o servidor sair da máquina dedicada para uma de uso geral.

**Por onde começar, se o dia chegar** (nenhuma projetada agora, em ordem de custo): bind em `127.0.0.1` mais um túnel SSH para acesso remoto; ou autenticação básica HTTP só na rota `/ota`, deixando `GET /` livre para leitura; ou senha de AP por device derivada do MAC em vez do `12345678` fixo — esta última é firmware, não backend.

### L5 — Nome do arquivo CSV vem do IP do peer

`{ip}_{timestamp}.csv`. Um peer IPv6 produz dois-pontos no nome, o que quebra a cópia via gvfs e qualquer manipulação do lado Windows. Mesma forma do servidor de produção — DEC-0 manda deixar como está; registrado para não ser redescoberto.

### L6 — Tamanhos pedidos ao socket — **RESOLVIDO** (commit `541fc37`)

**O problema que era:** praticamente toda a suíte de `handle_client` é insensível ao número de bytes que o `recv_exact` pede, porque `MagicMock` ignora o argumento e devolve o próximo item do `side_effect` independentemente do que se peça. Por muito tempo a única coisa segurando essa propriedade foi `test_recv_exact_requests_only_the_missing_bytes` — apagasse esse teste numa faxina e um `recv_exact` pedindo quantidade errada passava na suíte inteira.

**O que fechou:** `tests/test_integration_socket.py`, uma troca completa sobre socket de loopback real. Um socket real **não consegue** ignorar um tamanho pedido, e a direção danosa é a de **pedir demais**: `recv_exact(conn, 4)` que pedisse 990 receberia os 60 bytes disponíveis de uma vez, consumindo header, payload e bateria juntos — `remaining` fica negativo, o laço encerra, e a função devolve 60 bytes onde deviam vir 4. O `expected` sai como lixo e o teste falha. Nenhum mock percebe isso, porque o mock entrega o item enfileirado independentemente do pedido.

Pedir **de menos** é inofensivo: o laço continua até `remaining == 0`, só com mais chamadas. É a direção que não quebra nada, e por isso não precisa de pino.

Ponto único de falha **eliminado**, não mitigado: hoje dois testes independentes sustentam a propriedade, um contra mock e um contra o sistema operacional.

> **Limite da ferramenta, registrado onde importa.** Primeira versão desta nota dizia que o `mutmut` não gera mutação nenhuma de contagem de bytes. **Errado duas vezes, e as duas conferidas no corpo da função:** `remaining -= len(chunk)` (`tcp_server.py:118`) vira `+=` e vira `=` sob mutação de operador comum, o `mutmut` gera as duas, e **as duas produzem tamanho errado** — não mutando o argumento, mas corrompendo a aritmética do laço, de modo que todo `recv` seguinte pede o número errado. `+=` nunca termina; `=` pede errado já na segunda passada.
>
> Sobra a forma **estreita**, que é a que a L6 de fato guarda: o `mutmut` não expressa **"pediu ao socket um valor errado desde a primeira chamada"** — `conn.recv(remaining)` escrito como `conn.recv(n)`. Deslize humano comum, lê bem, quebra toda leitura fragmentada, e não é gerado porque os operadores da ferramenta invertem comparações, ajustam números e anulam nomes, mas **não substituem um local por outro**.
>
> A cegueira geral é outra e é estrutural: o que a ferramenta não enxerga é **um dublê que ignora os próprios argumentos** — propriedade do *arcabouço de teste*, não do código sob teste. Teste de mutação muta **código de produção**; dublê infiel é defeito **no teste**, logo nenhuma mutação da fonte pode revelá-lo.
>
> Exemplo que fecha o argumento: `conn.recv(remaining)` escrito como `conn.recv(n)`. Deslize humano comum, lê bem, quebra toda leitura fragmentada — e o `mutmut` não o gera, porque seus operadores invertem comparações, ajustam números e anulam nomes, mas não **substituem um local por outro**. Passa na suíte de mocks inteira. Falha contra socket real.
>
> Daí a formulação geral: nota de mutação mede se as **asserções** sustentam peso, não se os **dublês** são fiéis. Uma suíte de asserções perfeitas contra um mock que mente pontua 100%. E o corolário incômodo: julgado por mutantes mortos, o teste de integração seria cortado — ferramenta que pontua testes sempre subestima o teste que conserta as ferramentas.
