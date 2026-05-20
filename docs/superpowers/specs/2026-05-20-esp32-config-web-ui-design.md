# ESP32 Config Web UI — Design Spec

**Data:** 2026-05-20

## Objetivo

Página web no servidor Python que permite editar os 4 parâmetros de configuração enviados ao ESP32 (`sleep_min`, `idle_min`, `max_acq`, `cooldown_sec`) sem alterar o código. A página também exibe o histórico das últimas conexões dos dispositivos.

## Escopo

- Config global (todos os dispositivos recebem os mesmos valores)
- Config em memória — reseta para defaults ao reiniciar o servidor (sem persistência em disco)
- Histórico das últimas 50 conexões (IP, horário, número de amostras, tensão da bateria)

## Arquitetura

### Estrutura de arquivos

```
backend/
  app_state.py                  # novo — estado compartilhado em memória
  web/
    server.py                   # novo — Flask app
    templates/
      index.html                # novo — UI split: config à esquerda, histórico à direita
  server/
    tcp_server.py               # modificado — usa AppState
  config/
    settings.py                 # sem mudança (defaults permanecem aqui)
  requirements.txt              # modificado — adiciona flask
```

### Threading

```
main
 ├── Thread: server_main()      # TCP, porta 12345 (existente, modificado)
 ├── Thread: web_server_main()  # Flask, porta 8080 (novo)
 ├── Thread: save_data()        # worker CSV (existente, sem mudança)
 └── Thread: exit_monitor()     # 'q' para parar (existente, sem mudança)
```

## Modelo de dados (`app_state.py`)

```python
@dataclass
class DeviceConfig:
    sleep_min:    int = DEFAULT_SLEEP_MIN       # 240
    idle_min:     int = DEFAULT_IDLE_MIN        # 20
    max_acq:      int = DEFAULT_MAX_ACQ         # 5
    cooldown_sec: int = DEFAULT_COOLDOWN_SEC    # 5

@dataclass
class ConnectionEntry:
    ip:         str
    timestamp:  datetime
    n_samples:  int
    battery_mv: int

class AppState:
    def __init__(self):
        self.config      = DeviceConfig()
        self.connections = deque(maxlen=50)
        self.lock        = threading.Lock()
```

`AppState` é instanciado uma vez no `__main__` e passado como argumento para `server_main()` e `web_server_main()`.

## Rotas Flask (`web/server.py`)

| Método | Rota      | Descrição |
|--------|-----------|-----------|
| `GET`  | `/`       | Renderiza `index.html` com config atual + histórico |
| `POST` | `/config` | Valida e atualiza `app_state.config`, redireciona para `/` |

**Validação no POST:** todos os 4 campos são inteiros positivos. Valores inválidos retornam HTTP 400 com mensagem de erro; a config não é alterada.

**Porta:** `8080`

## Modificações em `tcp_server.py`

1. `server_main(state: AppState)` — recebe o estado compartilhado como parâmetro
2. `handle_client(conn, addr, state: AppState)` — duas mudanças:
   - Lê `state.config` (com lock) para montar o `pack_server_config`
   - Após processar, grava `ConnectionEntry` em `state.connections` (com lock)

## Layout da UI (`index.html`)

Split horizontal:

- **Esquerda (1/3):** formulário com os 4 campos numéricos + botão "Salvar"
- **Direita (2/3):** tabela com histórico de conexões (IP, horário, amostras, bateria)

Tema escuro, sem framework CSS externo — CSS inline ou `<style>` no próprio template.

## Fluxo de dados

```
[Browser] POST /config
     │
     ▼
[Flask] valida → adquire lock → atualiza AppState.config → libera lock → redirect GET /
     │
     ▼
[ESP32 conecta]
     │
     ▼
[tcp_server] adquire lock → lê AppState.config → libera lock → envia ServerConfig ao ESP32
     │
     ▼
[tcp_server] adquire lock → grava ConnectionEntry → libera lock
```

## Dependências novas

| Pacote | Versão mínima | Motivo |
|--------|--------------|--------|
| `flask` | 3.x | Servidor HTTP e renderização de templates |

## O que não está no escopo

- Persistência da config em disco
- Configuração por dispositivo
- Autenticação na página web
- Suporte a múltiplos servidores simultâneos
