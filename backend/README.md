# Backend — ESP32 Config Server

Servidor Python que recebe dados do ESP32 via TCP, salva em CSV e expõe uma interface web para configurar os parâmetros enviados ao dispositivo.

---

## Pré-requisitos

- Python 3.10+
- WSL2 (se estiver no Windows)

---

## Instalação

```bash
cd backend

# Criar e ativar ambiente virtual
python3 -m venv .venv
source .venv/bin/activate   # Linux / WSL
# ou: .venv\Scripts\activate  (PowerShell nativo — não recomendado, usar WSL)

# Instalar dependências
pip install -r requirements.txt
```

---

## Executar o servidor

```bash
cd backend
source .venv/bin/activate
python -m server.tcp_server
```

O servidor sobe duas portas:

| Porta | Protocolo | Função |
|-------|-----------|--------|
| `12345` | TCP | Recebe dados do ESP32 |
| `8080`  | HTTP | Interface web de configuração |

Para parar: digite `q` + Enter no terminal.

---

## Interface web

Abra no navegador:

```
http://localhost:8080
```

> **WSL2:** o servidor roda dentro do WSL. Para acessar pelo Windows use o IP do WSL no lugar de `localhost`:
> ```powershell
> # No PowerShell, descubra o IP do WSL:
> wsl hostname -I
> # Exemplo: 172.22.92.157
> # Acesse: http://172.22.92.157:8080
> ```

### O que você pode fazer na interface

**Painel esquerdo — Configuração**

Edite os 4 parâmetros e clique **SALVAR**. O ESP32 receberá os novos valores na próxima conexão.

| Campo | Descrição | Padrão |
|-------|-----------|--------|
| Sleep (min) | Tempo de deep sleep entre ciclos | 240 |
| Idle (min) | Tempo sem trigger antes de dormir | 20 |
| Max Acq | Máximo de aquisições antes de dormir | 5 |
| Cooldown (s) | Intervalo mínimo entre triggers | 5 |

**Painel direito — Últimas Conexões**

Exibe as últimas 50 conexões dos dispositivos: IP, horário, número de amostras e tensão da bateria.

> **Nota:** a configuração é mantida em memória. Se o servidor reiniciar, os valores voltam ao padrão.

---

## Configuração do ambiente

As variáveis de ambiente (opcionais) ficam em `config/settings.py`:

```python
SERVER_IP   = os.getenv('SERVER_IP',   '0.0.0.0')
SERVER_PORT = int(os.getenv('SERVER_PORT', '12345'))
GDRIVE_PATH = os.getenv('GDRIVE_PATH', '...')
```

Para sobrescrever sem editar o arquivo:

```bash
SERVER_IP=192.168.1.50 SERVER_PORT=9000 python -m server.tcp_server
```

---

## Testes

```bash
cd backend
source .venv/bin/activate
python -m pytest tests/ -v
```

16 testes cobrindo: AppState, rotas Flask e protocolo TCP.

---

## Estrutura dos arquivos

```
backend/
  app_state.py          # Estado compartilhado em memória (config + histórico)
  web/
    server.py           # Flask: GET / e POST /config
    templates/
      index.html        # Interface web (tema escuro, layout split)
  server/
    tcp_server.py       # Servidor TCP (recebe dados do ESP32)
  protocol/
    packet.py           # Parsing do protocolo binário
  config/
    settings.py         # Defaults e variáveis de ambiente
  tests/                # Testes automatizados
```
