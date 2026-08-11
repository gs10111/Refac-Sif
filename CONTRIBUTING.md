# Contribuindo — Refac-Sif

> **Regra canônica:** `docs/regras-de-review.md` do repo `arq` (repositório local da máquina do
> projeto, sem remote; vigente desde 2026-07-22 por determinação do Autor, `bigboss`). Ela vale para
> **todo** PR do projeto, em qualquer repositório — este incluído.
>
> Este arquivo é a **adaptação** dela a este repo: os comandos que existem aqui, os caminhos daqui, e
> os dois pontos em que Refac-Sif é mais arriscado que os outros repos (não há CI; o repo é público).
> Onde os dois textos divergirem, vale o `arq`.

## Fluxo: branch → TDD → PR → review → merge

1. **`git fetch` e branch a partir da `main`.** A base se move enquanto se trabalha.
2. **TDD, com o vermelho observado.** Teste antes da implementação, provando o critério de aceite da
   spec — não algo parecido.
   - Firmware: `~/.platformio/penv/bin/pio test -e native`
   - Backend: `backend/venv/bin/python -m pytest -q` (145 testes, ~1 s)
   - Prova de que o teste **detecta**, não só passa: `pio test -e mutant_<nome>` — ali a **falha é a
     condição de sucesso** (`platformio.ini`, envs `mutant_*`; racional em `test/README.md`).
   - **Cole o vermelho no PR.** Aqui não existe CI para registrar isso por você (ver "Diferenças").
3. **Commite WIP cedo.** O worktree isola a árvore; WIP não-commitado é invisível para os outros
   agentes e para o `bigboss`.
4. **`gh pr create`.** O corpo diz explicitamente **o que não está incluído** — quem revisa não
   adivinha o recorte (§4.3 da canônica).
5. **Review de área registrada no PR:**
   ```
   gh pr review <nº> --comment --body "..."
   ```
   Corpo com o template:
   - **Revisor e chapéu** — quem revisou e com que papel (ex.: "subagente adversarial, SRE+Security")
   - **Escopo varrido** — os ângulos que tentou derrubar, em lista; não "olhei tudo"
   - **Achados** — cada um com `arquivo:linha` + cenário de reprodução; ou **"sem achados"**
     explícito, dizendo o que foi verificado
   - **Veredito** — APROVA / APROVA COM RESSALVAS / REPROVA; ressalva corrigida aponta o commit

   Quem escreveu **não** revisa o próprio código, e a revisão do Tech Leader **não substitui** as
   revisões de área. Review que ficou só no chat **conta como não entregue**. Em PR crítico
   (credencial, OTA, wire contract), o revisor é spawnado por **outro** agente, cada revisor em
   **worktree própria**.
6. **Merge: o Autor (`bigboss`).** O Tech Leader prepara, revisa e recomenda; não mergeia sozinho.

## Revisores por área — como a tabela cai neste repo

| PR toca em… | Revisores obrigatórios |
|---|---|
| credencial WiFi, senha do SoftAP de OTA, segredo em `Preferences`, arquivo git-ignored de credencial (DEC-2) | **Security** + **QA** |
| `platformio.ini`, `.github/**` (quando existir), bind/porta do backend (`SERVER_IP`, `SERVER_PORT`, `WEB_PORT`), provisionamento, deploy | **SRE** + **Security** |
| wire contract (`[4 B total][N × 18 B][2 B bateria]`, config de 10 B), `struct ServerConfig`, colunas do CSV, `stage`/`RTC_DATA_ATTR` | **Architect** + **QA** |
| `test/**`, `backend/tests/**`, envs `mutant_*` | **QA** |

## Sem rede em teste — o padrão daqui

O equivalente local ao `TBClient`:

- **Firmware:** as interfaces puras em `lib/hal/*.h` (`ITransport`, `IRadio`, `IClock`, `IAllocator`,
  `IKeyValueStore`, `IAccessPoint`, …). A dependência entra **injetada**; `env:native` roda com falso,
  nunca com hardware.
- **Backend:** socket mockado injetado — `exchange()` em `backend/tests/test_tcp_server.py:67`.
- **Exceção única, já documentada:** `backend/tests/test_integration_socket.py` usa socket real de
  loopback em porta 0 (o kernel escolhe), com a justificativa no docstring — só um socket de verdade
  prova contagem de bytes. Loopback justificado passa; qualquer coisa que **saia da máquina**, não.

## SOLID com endereço, aqui

- **SRP** — `lib/protocol/packet.*` só fala do formato do pacote; `lib/ota/*` só do armar/desarmar do
  OTA. Esse é o nível de disciplina esperado.
- **DIP** — `lib/hal/*.h` existe para isso: é o que torna o teste possível sem hardware.
- **OCP/LSP/ISP** — só com caso concreto. Não inventar abstração para satisfazer sigla.
- **Parse na fronteira, uma vez** (P2): os 10 B da config e os frames de 18 B se validam na borda;
  depois disso ninguém revalida.

## Fail-closed — casos deste repo

- `ps_malloc` do buffer de 700000 B: null-check → **halt + Serial** (D2). Não degradar em silêncio
  para um buffer menor.
- Credencial ausente: o build embarcado **falha alto** (DEC-2). Nunca compilar com um default.
- Config do servidor: a leitura exige os **10 bytes**; curto é erro, não leitura parcial.
- `except` que só loga e segue precisa justificar **por escrito** por que seguir é correto.

## Duas diferenças que aumentam o risco aqui

1. **Não há CI.** Não existe `.github/` — nem `back/ci`, nem `front/ci`, nem
   `security/gitleaks`. A saída dos testes vai **colada no PR**, e contra segredo no diff o olho do
   revisor é a única rede. Não há rede de segurança para burlar.
2. **O repo é público** (`github.com/gs10111/Refac-Sif`, DEC-2; o original é privado). Segredo
   commitado aqui é irreversível na prática — fica no histórico, em forks e em caches de terceiros.
   Bloqueio absoluto.

## O que trava o merge

1. Teste vermelho, ou ausência de teste onde a spec pede critério de aceite.
2. Review obrigatória (tabela acima) não registrada no PR.
3. PR que não diz o que **não** está incluído.
4. Achado de severidade alta em aberto.
5. Segredo no diff.
