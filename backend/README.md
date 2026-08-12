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

Ambas vêm do ambiente: `SERVER_PORT` e `WEB_PORT` (os valores acima são os
padrões). Um valor que não seja inteiro derruba o servidor no boot, em vez de
voltar em silêncio para o padrão.

### Publicação no ThingsBoard (opcional)

`SIF_MQTT_ENABLED=1` mais `SIF_MQTT_HOST` fazem o servidor publicar cada captura
num broker MQTT local, de onde o `thingsboard-gateway` a encaminha. O CSV e a
cópia para o Drive continuam iguais — a publicação é adição ao caminho de
gravação, nunca portão na frente dele. Ligado sem host, o servidor não sobe.
Detalhes, tópicos e instalação: [`gateway/README.md`](../gateway/README.md).

### Onde a configuração fica guardada

`SIF_DB_PATH` (padrão `sif.db`, no diretório de onde o servidor sobe) é o SQLite
com a configuração da frota. Ele é lido **por conexão**, então o que se salva na
página vale para o próximo sensor que transmitir, sem reiniciar nada. Um caminho
que não possa ser aberto derruba o servidor no boot: cair para memória em
silêncio faria a configuração salva sumir no restart seguinte, sem avisar
ninguém.

### Taxa de amostragem da frota

A taxa se troca **pela página** (bloco *Taxa de amostragem*). `SIF_SAMPLING_HZ`
continua existindo e define o valor de um banco **novo**: `200`, `100`, `50`,
`25` ou `12.5`, padrão `50`. Uma vez que o banco exista, quem manda é o que está
nele — a variável não sobrescreve o que o operador salvou.

```bash
SIF_SAMPLING_HZ=200 python -m server.tcp_server
```

O sensor grava a taxa nova na NVS quando ela muda e a adota **no boot
seguinte** — a captura em curso termina na taxa antiga. Uma taxa que o
ICM-42688-P não roda derruba o servidor no boot: cair para 50 Hz em silêncio
daria uma frota amostrando numa taxa que ninguém escolheu e ninguém vê.

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

Os quatro são inteiros de 1 a 65535 — é a largura do campo no protocolo, não uma escolha nossa. Valor recusado devolve uma mensagem por campo dizendo qual é e por quê, e **nenhum** valor é gravado enquanto houver um errado.

O bloco **Taxa de amostragem**, logo abaixo, mostra em que taxa a frota está e troca-a por um seletor (200, 100, 50, 25 ou 12,5 Hz). Tem rota própria (`POST /sampling`) pelo mesmo motivo do OTA: um SALVAR distraído não pode re-taxar a planta inteira. Cada sensor grava a taxa nova e a adota **no boot seguinte** — a captura em curso termina na taxa antiga.

O bloco **Atualizacao OTA**, mais abaixo, também é independente do SALVAR: salvar a configuração nunca arma nem desarma o OTA. Ver a seção adiante.

**Painel esquerdo — Ocorrencias**

Lista as últimas 20 falhas que o servidor encontrou, mais recente primeiro:
config que não chegou ao device, captura recusada pelo publisher, CSV que não
gravou, cópia para o Drive que falhou, timeout de sensor, erro ao salvar a
configuração. Vermelho é erro, âmbar é aviso. Servidor sem falha nenhuma diz
`Nenhuma ocorrencia registrada desde que o servidor subiu`, em vez de mostrar
uma área vazia que parece painel quebrado.

Antes disso tudo isso ia só para o terminal do servidor: a página parecia
saudável enquanto capturas eram recusadas. As ocorrências ficam **em memória**
(as últimas 200) — restart limpa, e não substituem o log do serviço.

> **Atenção:** as mensagens trazem IP do device, nome de arquivo e o texto do
> erro, e a página **não tem autenticação** — quem alcança a rede do servidor lê
> tudo isso. Não é informação nova (a tabela de conexões já mostra os IPs), mas
> agora inclui texto de exceção. Mesmo risco da seção de OTA.

**Painel direito — Últimas Conexões**

Exibe as últimas 500 conexões dos dispositivos: IP, horário, número de amostras, tensão da bateria e se aquela conexão levou o comando de OTA.

A linha que levou o OTA fica destacada (fundo avermelhado, borda à esquerda) e recebe o selo **OTA**. É o único registro de qual sensor está prestes a reiniciar em modo Access Point — vale anotar o IP antes de sair da mesa.

> **O que sobrevive a um restart:** a **configuração** (os quatro campos e a taxa) fica em SQLite e volta igual. O **histórico de conexões**, as **ocorrências** e o **armamento do OTA** continuam em memória — reiniciou, histórico e ocorrências vazios, OTA desarmado.

---

## Atualização de firmware pelo ar (OTA)

O painel esquerdo tem o bloco **Atualizacao OTA**, com dois estados:

| Estado | O que a página mostra | Botão |
|---|---|---|
| desarmado | `OTA desarmado` | **ARMAR OTA** |
| armado | `OTA ARMADO — proximo device que transmitir`, em vermelho | **DESARMAR** |

O armamento é **de uso único**: o comando vai para o **próximo** device que completar uma transmissão, e o servidor desarma sozinho em seguida. O device seguinte já recebe o comando normal. Não existe forma de escolher **qual** device recebe — quem transmitir primeiro leva.

### Procedimento de campo

1. Na rede da planta, abra `http://<servidor>:8080` e clique **ARMAR OTA**. Confirme que a faixa ficou vermelha.
2. **Force uma transmissão no sensor que você quer atualizar**: passe o ímã. Sem isso a espera pode chegar ao tempo de sleep configurado (padrão 240 min), e nesse meio tempo qualquer outro sensor que transmitir leva o OTA no seu lugar. Este passo é o único controle sobre *qual* device é atualizado.
3. O device transmite, recebe o comando e **reinicia como Access Point**: rede `Update driver - <MAC>`, senha `12345678`.
4. Confira no histórico qual IP ficou com o selo **OTA** — é esse o device que saiu da rede.
5. **Saia do WiFi da planta** e conecte o notebook nesse Access Point. Enquanto estiver nele, o servidor fica inacessível.
6. Abra `http://192.168.4.1/` e envie o arquivo `.bin`. `/version` mostra a versão atual; `/restartESP` reinicia.
7. **A janela é de 5 minutos.** Se estourar, o device reinicia sozinho no modo normal, **e o armamento já foi consumido** dos dois lados — é preciso voltar ao passo 1 e esperar a próxima transmissão.
8. Volte o notebook para a rede da planta e confirme no histórico que o device voltou a transmitir.

> **Atenção:** armar o OTA não exige senha nenhuma. Qualquer pessoa com acesso à rede do servidor pode fazê-lo. Ver `docs/backend/ota-and-protocol-design.md`, seção 7, L4.

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

239 testes cobrindo: empacotamento do protocolo, leitura exata do socket, AppState e armamento de OTA, servidor TCP, escrita do CSV e rotas Flask.

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
