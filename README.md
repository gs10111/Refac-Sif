# SIF — Sistema Inercial de Fadiga

Sensor de vibração numa esteira industrial. Um ESP32-PICO com IMU ICM-42688-P acorda quando um ímã passa, grava as amostras na PSRAM, manda tudo por TCP para um servidor Python, recebe a configuração de volta e volta a dormir.

Este README é para quem acabou de clonar e quer **rodar**. Cada passo diz o comando, o que esperar, e o que fazer quando não funcionar.

```
   ímã passa                                          CSV local (+ cópia p/ Drive)
      │                                                        ▲
      ▼                                                        │
  ┌────────┐   TCP :12345    ┌──────────────────┐──────────────┘
  │ ESP32  │ ──────────────▶ │  servidor Python │
  │  +IMU  │ ◀────────────── │                  │──────▶ MQTT ─▶ ThingsBoard (opcional)
  └────────┘   config 12 B   └──────────────────┘
                                     │ HTTP :8080
                                     ▼
                             página de configuração
```

Você pode rodar **só o servidor** (sem hardware nenhum) ou **os dois lados**. Comece pelo servidor: ele é o que dá para testar na hora.

---

## Índice

1. [O que você precisa ter instalado](#1-o-que-você-precisa-ter-instalado)
2. [Rodar o servidor](#2-rodar-o-servidor-sem-hardware)
3. [Usar a página de configuração](#3-usar-a-página-de-configuração)
4. [Rodar os testes](#4-rodar-os-testes)
5. [Compilar e gravar o firmware](#5-compilar-e-gravar-o-firmware)
6. [Ver o sensor funcionando](#6-ver-o-sensor-funcionando-de-ponta-a-ponta)
7. [ThingsBoard (opcional)](#7-thingsboard-opcional)
8. [Quando não funciona](#8-quando-não-funciona)
9. [Mapa do repositório](#9-mapa-do-repositório)

---

## 1. O que você precisa ter instalado

| Ferramenta | Para quê | Como conferir |
|---|---|---|
| **Python 3.10+** | servidor e testes | `python3 --version` |
| **PlatformIO Core** | compilar e gravar o firmware | `pio --version` |
| **Git** | clonar | `git --version` |

Só o Python é obrigatório para começar. PlatformIO só entra quando você for mexer no firmware.

**Instalar o PlatformIO**, se for o caso:

```bash
python3 -m pip install --user platformio
```

Se o comando `pio` não aparecer depois disso, ele está em `~/.platformio/penv/bin/pio` — use o caminho completo ou acrescente ao `PATH`.

> **Windows:** rode tudo por **WSL2**. O servidor funciona no PowerShell, mas os caminhos deste README assumem Linux, e a gravação do ESP32 via USB é mais simples pelo Windows nativo — se for gravar, use o PlatformIO do VS Code no Windows e deixe o servidor no WSL.

---

## 2. Rodar o servidor (sem hardware)

```bash
git clone git@github.com:dieletrons/Refac-Sif.git
cd Refac-Sif/backend

python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
```

Suba:

```bash
python -m server.tcp_server
```

**O que você deve ver:**

```
2026-08-13 10:00:00,000 - INFO - Configuration stored in sif.db
2026-08-13 10:00:00,001 - INFO - Server listening on 0.0.0.0:12345
Type 'q' to stop.
 * Running on http://127.0.0.1:8080
 * Running on http://192.168.1.100:8080
```

Duas portas sobem juntas:

| Porta | O quê |
|---|---|
| `12345` | TCP, onde o ESP32 conecta |
| `8080` | HTTP, a página de configuração |

Para parar: digite `q` e Enter.

> **A porta 8080 é a que mais dá conflito** (Tomcat, ThingsBoard, outro Flask). Se aparecer `Address already in use`, suba assim:
> ```bash
> WEB_PORT=8081 python -m server.tcp_server
> ```
> Sem isso o TCP fica de pé e **só a página morre** — o que confunde, porque o servidor parece estar funcionando.

Um arquivo `sif.db` aparece no diretório de onde você subiu: é a configuração da frota, em SQLite. Ele sobrevive ao restart, e é isso que faz o que você salva na página continuar valendo.

---

## 3. Usar a página de configuração

Abra `http://localhost:8080` (ou o IP da máquina, se estiver noutro computador).

**Painel esquerdo — o que você controla:**

| Bloco | O que faz | Vale a partir de |
|---|---|---|
| Configuração | `sleep_min`, `idle_min`, `max_acq`, `cooldown_sec` | próxima conexão do sensor |
| Taxa de amostragem | 200, 100, 50, 25 ou 12,5 Hz | próxima **aquisição** do sensor |
| Atualização OTA | arma o modo de gravação sem fio | próximo sensor que transmitir |
| Ocorrências | as últimas 20 falhas do servidor | — |

Cada bloco tem botão próprio de propósito: salvar a configuração **nunca** troca a taxa nem arma o OTA.

**Painel direito — o que aconteceu:** as últimas 500 conexões, com IP, horário, número de amostras, bateria, **versão do firmware** e **Hz medidos**. Essa última coluna é a taxa que o sensor *atingiu*, não a que você pediu — é por ela que se confirma que uma troca de taxa pegou.

> Configuração persiste. Histórico de conexões, ocorrências e armamento de OTA **não** — restart zera os três.

---

## 4. Rodar os testes

Você não precisa de hardware para nenhum deles.

**Servidor** (de dentro de `backend/`, com a venv ativa):

```bash
python -m pytest -q
```
Esperado: `259 passed` em ~16 s.

**Gateway** (da raiz):

```bash
backend/venv/bin/python -m pytest gateway -q
```
Esperado: `11 passed`. Rodam sem broker e sem o `thingsboard-gateway` instalado.

**Firmware, na sua máquina, sem ESP32:**

```bash
pio test -e native
```
Esperado: `113 test cases: 113 succeeded`. Toda a lógica pura — máquina de estados, buffer circular, protocolo, política de taxa — roda aqui porque o que toca o hardware fica atrás das interfaces de `lib/hal/`.

**Testes que provam que os testes funcionam:** o repositório tem "mutantes" — implementações deliberadamente quebradas atrás de flags. Ali, **falhar é o resultado certo**:

```bash
pio test -e mutant_sampling_trusts_server
```
Se isso passar todo verde, alguém removeu a proteção em vez do bug. Lista completa em [`test/README.md`](test/README.md).

---

## 5. Compilar e gravar o firmware

### 5.1 Credenciais de WiFi

O build **falha de propósito** sem elas:

```bash
cp include/secrets.example.h include/secrets.h
$EDITOR include/secrets.h
```

Preencha:

```c
#define WIFI_SSID     "nome-da-rede"
#define WIFI_PASSWORD "senha-da-rede"
```

> **Três armadilhas aqui — as três já custaram tempo de bancada:**
> 1. O arquivo é git-ignored e nunca deve ser commitado — este projeto tem um espelho público.
> 2. Deixar os placeholders **compila limpo** e gera um sensor que grava, arranca, amostra e nunca alcança a rede. Se o serial mostrar cinquenta pontinhos seguidos de timeout, é isso.
> 3. O ESP32 **não enxerga redes de 5 GHz**. A rede tem que ser 2,4 GHz.

### 5.2 Endereço do servidor

Em [`src/config/network_config.h`](src/config/network_config.h), confira:

```c
#define SERVER_HOST "192.168.1.100"   // IP da máquina que roda o servidor
```

E o IP fixo do próprio sensor, logo abaixo. Isso é topologia, não segredo, então é versionado — mudou de rede, muda aqui e regrava.

### 5.3 Compilar e gravar

```bash
pio run -e pico32                  # só compila
pio run -e pico32 -t upload        # compila e grava pelo USB
pio device monitor -b 115200       # abre o serial
```

Placa: `pico32` (ESP32-PICO com PSRAM). O `upload_speed` é 921600; se a gravação falhar no meio, baixe para 460800 no [`platformio.ini`](platformio.ini).

---

## 6. Ver o sensor funcionando de ponta a ponta

Com o servidor de pé e o firmware gravado, passe um ímã pelo reed switch (GPIO 33). O serial deve mostrar, nesta ordem:

```
Taxa de amostragem: codigo ODR 9          ← boot: 9 = 50 Hz
Button Pressed!                            ← o ímã foi detectado
7926                                       ← duração da aquisição, em ms
Conectado ao Wi-Fi.
RSSI -41 dBm, IP 192.168.1.118
Conectando ao servidor TCP...
Conectado ao servidor.
Escrita: 106512 de 106512 bytes aceitos em 497 ms (1714.48 kbps)
Transmissão concluída: 497 ms de escrita, 34 ms de espera, 12 de 12 bytes de resposta
Recebido do servidor: 240, 20, 5, 5, 0, taxa 9
```

E no servidor:

```
INFO - Connected: 192.168.1.118:49820
INFO - Config sent to 192.168.1.118 (update=0)
INFO - Saved 192.168.1.118_20260813_100000.csv
```

**Entre esses blocos o serial sai embaralhado, e isso é normal.** Durante a coleta a CPU cai para 10 MHz e o divisor de baud vai junto. Volta a ser legível na transmissão, a 240 MHz.

### Confirmar que uma troca de taxa pegou

1. Na página, troque a taxa e salve
2. Passe o ímã
3. O serial diz `Nova taxa gravada: codigo ODR 7. Vale a partir da proxima aquisicao.`
4. Passe o ímã de novo — a coluna **Hz medidos** da página mostra a taxa nova

Conferência independente, pelo CSV:

```bash
python3 -c "
import csv,sys,statistics
r=list(csv.DictReader(open(sys.argv[1])))
d=[int(r[i+1]['timestamp'])-int(r[i]['timestamp']) for i in range(len(r)-1)]
print(f'{1000/statistics.median(d):.1f} Hz')
" 192.168.1.118_*.csv
```

---

## 7. ThingsBoard (opcional)

Desligado por padrão. Ligando, cada captura também é publicada num broker MQTT local, de onde o `thingsboard-gateway` a encaminha:

```bash
SIF_MQTT_ENABLED=1 SIF_MQTT_HOST=127.0.0.1 python -m server.tcp_server
```

O CSV e a cópia para o Drive continuam iguais — publicar é adição ao caminho de gravação, nunca portão na frente dele. Ligado sem `SIF_MQTT_HOST`, o servidor **não sobe**.

Instalação do broker e do gateway: [`gateway/README.md`](gateway/README.md).

---

## 8. Quando não funciona

| Sintoma | Causa quase certa | O que fazer |
|---|---|---|
| `Address already in use` na 8080 | outra coisa ocupa a porta | `WEB_PORT=8081 python -m server.tcp_server` |
| Cinquenta pontinhos e timeout no serial | SSID/senha errados, ou rede 5 GHz | conferir `include/secrets.h`; o firmware imprime o motivo na linha seguinte |
| `errno 104` ao conectar no TCP | ninguém escutando na 12345 | subir o servidor; conferir com `ss -ltnp \| grep 12345` |
| `Timeout from <ip>` no servidor | o device abriu e não mandou tudo | a mensagem diz quantos bytes chegaram de quantos |
| Serial vira lixo depois do boot | CPU a 10 MHz na coleta | normal — volta na transmissão |
| Troquei a taxa e nada mudou | vale da **próxima aquisição** | passe o ímã de novo e olhe *Hz medidos* |
| Sensor transmite e não gera CSV | aquisição encerrada por `idle_min` | é o projeto: sem ímã, a captura é descartada |
| `#error "Missing include/secrets.h"` | credenciais ausentes | passo [5.1](#51-credenciais-de-wifi) |
| `ps_malloc` falhou, device parado | PSRAM indisponível | conferir se a placa é mesmo `pico32` com PSRAM |

O bloco **Ocorrências** da página mostra as falhas do servidor sem você abrir terminal nenhum.

---

## 9. Mapa do repositório

```
src/          código que só roda no ESP32 (Arduino, WiFi, SPI, NVS)
  app/          orquestra o boot e o ciclo
  services/     implementações concretas: rádio, TCP, NVS, IMU
  config/       IP do servidor e da placa  (secrets.h vem à parte)
lib/          lógica pura, testável sem hardware
  hal/          as interfaces que src/ implementa
  acquisition/  coleta, timeout de esteira parada, aplicação da taxa
  ringbuffer/   buffer circular de 60.000 frames
  protocol/     formato dos pacotes, envio, leitura da config
  cycle/        uma iteração: coletar, transmitir, aplicar
  belt_cycle/   máquina de estados que sobrevive ao deep sleep
  ota/          armar, desarmar e entrar em modo de gravação sem fio
include/      constantes de placa e o secrets.h (não versionado)
test/         testes do firmware, rodam em env:native

backend/      servidor Python
  server/       TCP, CSV, cópia para o Drive
  web/          página de configuração (Flask)
  store/        configuração da frota em SQLite
  protocol/     o mesmo contrato de bytes, do lado de cá
  telemetry/    publicação MQTT
  tests/        259 testes

gateway/      ThingsBoard: converter, compose, broker
docs/fluxo/   página explicando o fluxo do sensor, com diagramas
```

**Antes de abrir PR**, leia [`CONTRIBUTING.md`](CONTRIBUTING.md): este projeto exige teste antes da implementação e review de área registrada no PR.

Para entender o comportamento do sensor em detalhe — máquina de estados, buffer circular, contrato de bytes, prazos — abra [`docs/fluxo/index.html`](docs/fluxo/index.html) no navegador.
