# Gateway — capturas do SIF no ThingsBoard

O servidor TCP continua sendo o front dos sensores e continua gravando o CSV
exatamente como antes. Este diretório acrescenta o caminho até o ThingsBoard:

```
ESP32 --TCP--> server.tcp_server --MQTT--> Mosquitto --> tb-gateway --> ThingsBoard
                     |
                     +--> CSV local (+ cópia para o Google Drive)
```

O CSV é o registro de última instância. Nada aqui pode custar o arquivo: se o
broker estiver fora, se o gateway estiver parado, se a captura for recusada — o
CSV é gravado do mesmo jeito.

## O que é publicado

| Tópico | Quando | Conteúdo |
|---|---|---|
| `sif/telemetry/<ip>` | uma mensagem por bloco de 100 amostras | uma entrada por amostra: `x_data`, `x_gyro`, `y_data`, `y_gyro`, `z_data`, `z_gyro`, `temp`, cada uma com o timestamp da própria amostra |
| `sif/burst/<ip>` | uma por captura | `battery_voltage`, `sample_count`, `duration_ms` |

O device no ThingsBoard é o **IP** do sensor — é a única identidade que o
dispositivo fornece. Consequência assumida: um sensor que volta com outro lease
de DHCP aparece como device novo, e um IP reutilizado aparece como o mesmo.

### Por que o timestamp precisa de âncora

O ESP32 reinicia `millis()` a cada deep sleep, então toda captura chega
começando perto de zero. A última amostra é ancorada no instante em que a
conexão fechou e as anteriores retrocedem pelos deltas medidos pelo próprio
sensor — o espaçamento real é preservado, e o histórico não fica com todas as
capturas empilhadas no mesmo segundo.

### Capturas recusadas

O enquadramento TCP pode dessincronizar e produzir timestamps caóticos (visto em
1 de 4 CSVs de laboratório, com valores fora de ordem e span de 36 dias).
Publicar isso espalharia datapoints por semanas de histórico compartilhado, de
forma irreversível. Dois critérios, ambos independentes da duração da captura:

- timestamps não-decrescentes;
- intervalo **médio** por amostra ≤ 1000 ms.

Recusa não perde dado: o CSV local tem o bruto para diagnóstico, e o motivo é
devolvido a quem chamou, não só logado.

## Subir o pipeline

**1. Servidor TCP publicando.** No `.env` do serviço (ver `.env.example`):

```bash
SIF_MQTT_ENABLED=1
SIF_MQTT_HOST=127.0.0.1
```

Sem `SIF_MQTT_ENABLED`, nada é publicado e o servidor roda como sempre rodou.
Com ele ligado e sem host, o servidor **não sobe** — um servidor que parece
configurado para o ThingsBoard e publica no vazio é pior que um que recusa
iniciar.

Instale a dependência: `pip install -r backend/requirements.txt` (traz o
`paho-mqtt`).

**2. Broker local.**

```bash
sudo cp mosquitto-sif.conf /etc/mosquitto/conf.d/sif.conf
sudo systemctl restart mosquitto
```

Ele escuta **só em loopback** e sem autenticação; é o que torna aceitável não
ter senha. Não abra na rede.

**3. Credenciais do gateway.**

```bash
cp tb_gateway.json.example tb_gateway.json   # preencha host e accessToken
cp .env.example .env                         # preencha o que for do seu ambiente
# ponha o ca.pem do seu ThingsBoard neste diretório
```

`tb_gateway.json`, `.env`, `ca.pem` e `gw-config/` são git-ignored. **Este
repositório é público e não tem `gitleaks` no CI** — o olho de quem revisa é a
única rede contra um token commitado.

**4. Configuração base da imagem.** O gateway grava estado no diretório de
configuração, então ele precisa vir da imagem e não do repositório:

```bash
docker create --name tmp-gw thingsboard/tb-gateway
docker cp tmp-gw:/thingsboard_gateway/config ./gw-config
docker rm tmp-gw
cp mqtt.json tb_gateway.json ./gw-config/
```

**5. Subir.**

```bash
docker compose -f docker-compose.gateway.yml up -d
docker logs -f tb-gateway
```

## Testes

```bash
backend/venv/bin/python -m pytest gateway -q
```

Rodam sem gateway instalado e sem broker: os imports do `thingsboard_gateway`
degradam para stubs e o cliente MQTT é injetado nos testes do publisher
(`backend/tests/test_telemetry_publisher.py`).

## O que este diretório NÃO faz

- **Não substitui o CSV nem a cópia para o Google Drive.** Ambos continuam.
- **Não guarda histórico consultável** — quem faz isso é o ThingsBoard. Não há
  SQLite aqui.
- **Não sobe o ThingsBoard.** O `docker-compose.gateway.yml` sobe só o gateway;
  o servidor ThingsBoard é externo.
- **Não autentica o broker.** Loopback e `allow_anonymous true`.
