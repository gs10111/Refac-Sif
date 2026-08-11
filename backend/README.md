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

O bloco **Atualizacao OTA**, logo abaixo, é independente do SALVAR: salvar a configuração nunca arma nem desarma o OTA. Ver a seção adiante.

**Painel direito — Últimas Conexões**

Exibe as últimas 500 conexões dos dispositivos: IP, horário, número de amostras, tensão da bateria e se aquela conexão levou o comando de OTA.

A linha que levou o OTA fica destacada (fundo avermelhado, borda à esquerda) e recebe o selo **OTA**. É o único registro de qual sensor está prestes a reiniciar em modo Access Point — vale anotar o IP antes de sair da mesa.

> **Nota:** a configuração é mantida em memória. Se o servidor reiniciar, os valores voltam ao padrão e o OTA volta desarmado.

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

111 testes cobrindo: empacotamento do protocolo, leitura exata do socket, AppState e armamento de OTA, servidor TCP, escrita do CSV e rotas Flask.

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
