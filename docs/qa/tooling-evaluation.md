# Tooling evaluation — mutation testing on the backend, emulation for the firmware

Question asked: not "is it good", but **what does it catch that we do not catch
today, and what does it cost**.

| Tool | Verdict | Cost | First step |
|---|---|---|---|
| **mutmut** (backend mutation testing) | **ADOPT NOW**, pinned to 2.5.1, string mutants disabled | minutes per run, one pinned dependency, one documented invocation | sama writes the test for survivor #94 |
| **QEMU (Espressif fork) + pytest-embedded** | **REJECT** | an ESP-IDF install plus a port off the Arduino framework, to answer 1 of 3 questions | none |
| **Wokwi + wokwi-cli** | **REJECT for the three questions asked. Revisit only for the trigger path in CI** | metered cloud service, alpha scenario API, firmware binary leaves the building | none until H6/H7 are done on a bench |

The finding that drives both firmware verdicts: **all three questions lave wants
answered are answerable on one pico32 on a bench in under an hour, at higher
fidelity than any emulator gives.** Emulation is being asked to substitute for
hardware access we already have.

---

## Tool 1 — mutmut

### What was run

Everything below is measured on this branch, not read about.

```
# baseline
cd backend && ./.venv/bin/python -m pytest tests/ -q     # 111 passed in 0.75s (1.557s wall)

# mutmut 3.6.0 — FAILED, see below
# mutmut 2.5.1 — works:
./.venv/bin/mutmut run \
  --paths-to-mutate "app_state.py,config,protocol,server,web" \
  --runner "./.venv/bin/python -m pytest -x -q tests/" \
  --disable-mutation-types=string
```

### Version: 2.5.1, pinned, and this is a real cost

`pip install mutmut` resolves **3.6.0, which does not work for this layout**. 3.x
requires `source_paths` in `setup.cfg`/`pyproject.toml` (neither existed), then
copies the tree into `backend/mutants/` and runs pytest from there — which breaks
every import, because the suite reaches `app_state` and `protocol.packet` through
pytest's rootdir. Observed result: `0 files mutated`, then `ModuleNotFoundError`.

2.5.1 takes `--paths-to-mutate` on the command line, needs no config file inside
sama's area, and ran first time.

**Price this honestly:** 2.5.1 is an old release of a tool whose current major
version we have established does not fit. We are adopting an unmaintained pin. That
is acceptable for a nightly quality job — if it ever breaks we lose a report, not a
build — and it would not be acceptable for anything in the critical path.

### Results

With string mutants disabled:

| | Count |
|---|---|
| Mutants generated | **187** |
| Killed | 115 |
| Timeout | 3 |
| Survived | **69** |
| **Mutation score** | **61.5%** (115/187), or 63.1% counting timeouts as detections |

Runtime: a few minutes with a warm cache. **Comfortably inside the loop, not a
nightly job** — the suite is 0.75 s, and that is what makes this affordable. A
1-minute suite would have put this at three hours and the verdict would have been
"nightly, on main only".

### `--disable-mutation-types=string` is not optional

The first run, with string mutants on, produced 92 survivors. Sampling them showed
the population:

```
#102  logging.info(f"Saved {filename}")  ->  f"XXSaved {filename}XX"   noise
#141  addr[0] -> addr[1]  inside a warning message                     noise
#152  same shape                                                       noise
```

Log-string mutants are unkillable by design — we do not assert on log text and we
should not start. Left on, they are 25% of the survivor list and they train the
reader to skim it, which is how the real ones get missed. Disabled, the survivor
count drops 92 → 69 and every remaining row is at least arguable.

### What it caught that we do not catch today

**Survivor #94 — `saved = False` → `saved = True` in `save_data`, survived.**

This is the find, and it is worth the whole exercise. It means **no test covers "the
local CSV write failed, so do not attempt the Drive copy"** — the `if saved` guard
from the B6 fix, shipped today, with its failure path untested. All three B6 tests
take the path where the write succeeded. A five-minute test for sama, and worth more
than the other 68 put together.

**Timeout #89 — `break` → `continue` on the `None` sentinel in `save_data`.**

Careful with this one: it **timed out, which is a detection, not a miss.** The mutant
made the writer thread spin forever and the suite hung; mutmut reports timeouts
separately for exactly that reason. The narrower claim that survives is still worth
acting on: *nothing asserts that the sentinel terminates the writer thread*, so the
property is caught only by wall clock — which in CI reads as flakiness rather than as
a failure, and gets retried rather than investigated.

### Does it agree with our hand-made mutation?

**Yes on the behaviour, no on the mutant, and the disagreement is instructive.**

mutmut has **no statement-deletion operator**, so it never generates our hand mutation
("delete `self.ota_armed = False` from `take_config_for_send`"). What it generates at
that line is the keyword flip `False → True`, which is operationally the same defect —
the flag is never cleared, so every device gets `update=1`.

Run against `app_state.py` alone, cache dropped:

```
18/18   killed 18   timeout 0   suspicious 0   survived 0
```

**Eighteen mutants, zero survivors.** The only file in the backend where nothing at
all gets past the suite is the file we hand-mutated. Both methods agree, and the claim
now has a denominator — "zero survivors out of eighteen" is a result; "zero survivors"
alone is not.

The lesson generalises to the firmware harness: an automated mutation tool and a
hand-authored one are not substitutes. Ours can express "delete the line that clears
the flag" and mutmut cannot; mutmut generates 187 mutants across five modules and we
would never hand-write 187. **Different instruments, same dial.**

### Verdict: ADOPT NOW

| For | Against |
|---|---|
| Found a real, shipped-today hole on first run | Pinned to an unmaintained 2.5.1 |
| Minutes, not hours — fits in the loop | Needs the string filter or it is noise |
| 61.5% is a real baseline to hold a line against | Score is a bad target if anyone starts optimising it |
| Cross-checks our hand-made mutation | 69 survivors is a backlog nobody has triaged yet |

**Concrete first steps, in order:**

1. sama writes the failing test for #94 (local write fails → no Drive copy attempted).
2. Add a test asserting the sentinel terminates the writer thread, so #89 is caught by
   an assertion instead of a clock.
3. Record the invocation in `backend/README.md` — pinned version, the
   `--disable-mutation-types=string` flag, and *why* both are there. An undocumented
   pin is a trap for the next person.
4. Triage the 69 by file, once. `config/settings.py` survivors are almost certainly
   constants with no behavioural meaning; `server/tcp_server.py` is where the real ones
   will be.
5. **Do not put a mutation-score gate in CI.** 61.5% is a baseline to watch, not a
   number to hit. A score target turns into tests written against mutants rather than
   against behaviour — the firmware harness's own rule ("choose the mutation before you
   see which tests are weak") exists for the same reason.

`.mutmut-cache` is deleted and should stay untracked. A nightly run wants a cold cache
anyway.

---

## Tool 2 — QEMU, pytest-embedded, Wokwi

### Starting position: nothing is installed

```
which qemu-system-xtensa qemu-system-riscv32 wokwi-cli idf.py   -> all four absent
ls -d ~/.espressif ~/esp                                        -> neither exists
~/.platformio/packages -> contrib-piohome, framework-arduinoespressif32,
                          toolchain-xtensa-esp32, tool-esptoolpy, tool-mkfatfs,
                          tool-mklittlefs, tool-mkspiffs, tool-scons
```

No QEMU, no ESP-IDF, no wokwi-cli, and PlatformIO's package set is **Arduino-framework
only**. So unlike Tool 1, nothing here could be evaluated by running it — the cost of
producing *any* evidence is an installation first. The evaluation below is therefore
against primary sources, and every claim is labelled with how it was established.

### The three questions, answered

| # | Question | QEMU | Wokwi | Bench |
|---|---|---|---|---|
| **Q1** | Does the OTA flag survive `ESP.restart()`? (NVS) | plausible | plausible | **yes, definitive** |
| **Q2** | Is R9 real — `RTC_DATA_ATTR` retained across deep-sleep wake, reloaded on other resets? | **no** | probable | **yes, definitive** |
| **Q3** | Can a deep sleep and its wake cause be observed at all? | **no** | probable | **yes, definitive** |

### QEMU — rejected, and two of three questions are structurally dead

*Established from Espressif's own QEMU documentation and the ESP32 forum; not run.*

- **Deep sleep crashes.** Calling `esp_deep_sleep_start()` under the Espressif QEMU
  fork produces `Guru Meditation Error: Core 0 panic'ed (LoadStorePIFAddrError)`,
  traced to the RTC sleep init. **Q2 and Q3 are not "low fidelity" here — they cannot
  be asked at all.**
- **No WiFi emulation.** Espressif's peripheral list covers crypto, Ethernet (OpenCores
  MAC), GPIO strapping, eFuse, SPI flash, SD/MMC, timers. WiFi is absent. Our entire
  transmit path is out of reach.
- **RTC watchdog not emulated** — stated explicitly.
- **PSRAM is better than expected and it does not rescue this.** PSRAM *is* emulated as
  memory (`-m 2M` / `-m 4M`); only the PSRAM **MMU** is missing, so bank switching and
  himem do not work. Our 700000 B allocation is ~683 KB, well under 2 MB, and needs no
  bank switching — so `ps_malloc(700000)` would plausibly work in QEMU. lave's
  assumption that PSRAM caps the whole approach turns out to be **wrong**; it is deep
  sleep and WiFi that kill it, not memory.
- **Framework mismatch.** `pytest-embedded-qemu` is built around the ESP-IDF build
  layout. This project is `framework = arduino` under PlatformIO. Bridging that is real
  work before the first assertion runs.

So QEMU would cost an ESP-IDF installation plus a build-system port, to answer **one**
of three questions — the one a bench answers in ten minutes.

### Wokwi — capable, and rejected anyway for these questions

*Established from Wokwi's documentation and public projects; not run.*

What it genuinely does that QEMU cannot:

- **Deep sleep works**, including `esp_sleep_enable_ext0_wakeup` and
  `esp_sleep_get_wakeup_cause()` returning `ESP_SLEEP_WAKEUP_EXT0`. Public projects use
  `esp_sleep_enable_ext0_wakeup(GPIO_NUM_33, ...)` — **our exact pin**.
- **PSRAM supported** on ESP32 via a `psramSize` attribute (2/4/8 MB).
- **WiFi supported**, unlike QEMU.
- **GPIO with interrupts supported.**

**The GPIO33 question, answered directly:** yes, `wokwi-cli` can script a reed-switch
edge — but **not** by driving a raw pin level, which is what the question assumes. There
is no "set pin 33 low" step. Scenarios drive a *part's control*:

```yaml
steps:
  - set-control: { part-id: reed, control: pressed, value: 1 }
  - delay: 100ms
  - set-control: { part-id: reed, control: pressed, value: 0 }
  - wait-serial: 'Coletando dados'
```

Model the reed as a `wokwi-pushbutton` or slide switch wired to GPIO33 and you have a
scriptable, CI-runnable magnet pass. Full step vocabulary: `delay`, `set-control`,
`wait-serial`, `write-serial`, `expect-pin`, `take-screenshot`, `touch*`.

Why it is still a reject **for these three questions**:

1. **It cannot close the transmit path.** Wokwi CI runs in **Wokwi's cloud**. Our device
   takes static IP `192.168.1.118` and connects to `192.168.1.100:12345` — a server on
   our LAN. A cloud simulation cannot reach it. The half of the duty cycle that is
   currently least tested is the half Wokwi cannot help with.
2. **The firmware binary leaves the building.** Today that is harmless. Once DEC-2 lands
   and the real plant SSID and password are compiled into the image, uploading that
   binary to a third-party cloud service becomes a credential-disclosure decision, not a
   tooling decision. **That is bigboss's call, not QA's, and it should be made before we
   build a dependency on it, not after.**
3. **Metered and alpha.** Free tier is 50 simulation-minutes/month; the scenario API is
   documented as alpha and subject to change. Fine for an experiment, thin for something
   the release depends on.
4. **A simulator answering Q2 is weak evidence for Q2.** The question is whether *the
   silicon* reloads RTC memory on a non-deep-sleep reset. A simulator that agrees with
   the documentation tells us the simulator implements the documentation. The failure
   mode we care about — silicon behaving unlike the docs on some reset path — is exactly
   the one emulation cannot surface.

### What to do instead

H6 and H7 from `test-plan.md`, on one pico32, with a serial print. Concretely:

```
Q2/R9:  print `stage` at boot, then exercise four resets —
        timer deep sleep, ESP.restart(), power cycle, EN pin.
        Expect: retained on the first, initial value on the other three.
Q1/H7:  arm OTA, let a device transmit, watch it restart, confirm the AP appears;
        then power-cycle mid-window and confirm it does NOT come back into OTA.
Q3:     esp_sleep_get_wakeup_cause() printed at boot — timer vs ext0, directly.
```

Under an hour, on the actual chip, answering all three at a fidelity no emulator
offers. **Fifteen minutes of that hour closes the most load-bearing unverified
assumption on the branch.**

### When to revisit Wokwi

One case is genuinely strong, and it is not on lave's list: **putting the
trigger-stops-acquisition path in CI.** A scripted GPIO33 edge is the closest thing to
testing the belt without a belt, and no host test can reach the `digitalRead` →
`BeltTrigger::poll` → `AcquisitionService::step` loop in `app.cpp:96-152`, which is
precisely the Arduino-side code round 9 is meant to extract.

Revisit if **all** of these hold:

1. Round 9 has extracted the cycle loop, and it is *still* worth testing the wired
   version — if extraction makes it host-testable, Wokwi buys nothing.
2. bigboss has ruled on shipping a credential-bearing binary to a third-party cloud.
3. Someone wants the belt path in CI badly enough to fund a metered dependency.

Until then the honest position is that we have hardware, we have not used it for these
questions yet, and buying an emulator before using the bench would be paying to avoid a
walk to the lab.

---

## Sources

Tool 1 is measured on this branch. Tool 2's capability claims are from:

- [Espressif QEMU — ESP32 emulation status](https://github.com/espressif/esp-toolchain-docs/blob/main/qemu/esp32/README.md) — peripheral list, PSRAM MMU limitation, RTC watchdog
- [ESP-IDF QEMU guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/tools/qemu.html) — flash image handling
- [ESP32 forum — QEMU crash on deep sleep](https://esp32.com/viewtopic.php?t=35904) — `LoadStorePIFAddrError` in `esp_deep_sleep_start`
- [pytest-embedded](https://docs.espressif.com/projects/pytest-embedded/en/latest/) — plugin set incl. `-qemu`, `-arduino`, `-wokwi`
- [Wokwi ESP32 simulation features](https://docs.wokwi.com/guides/esp32) — PSRAM, WiFi, GPIO support matrix
- [Wokwi automation scenarios](https://docs.wokwi.com/wokwi-ci/automation-scenarios) — full step vocabulary
- [Wokwi CI](https://docs.wokwi.com/wokwi-ci/getting-started) — cloud execution, metering
