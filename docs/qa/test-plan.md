# SIF — acceptance test plan

Scope: the D1 duty cycle, the wire contract, and the OTA feature. This is an
**acceptance document**, not a harness proposal — the harness exists
(`pio test -e native`, Unity, 48 host tests, 6 mutation envs; `backend/.venv`
pytest, 111 tests). Written against commit `1184dd3` (round 6 closed, rounds 7–8
open).

Status column:
- **host** — proven by a host test that exists today, named in the row.
- **struct** — guaranteed by the shape of the code, not by an assertion. Real but
  unenforced: the next edit can remove it silently.
- **HW** — cannot be proven off the chip. See [Hardware-in-the-loop](#2-hardware-in-the-loop).
- **open** — not covered, and the row says what would cover it.

---

## 0. DEC-0 as an acceptance criterion in its own right

> The original is in production and works, defects included.

Every behaviour this document calls **correct** must be justifiable as exactly one
of:

| Justification | Meaning |
|---|---|
| **matches-original** | Byte-identical, or observably identical, to `SIF-DI241794.../src/main.cpp` + `lib/ICM42688P/`. Cite the line. |
| **listed-regression** | A refactor-vs-original defect on the R-list. Cite the R number. |
| **new-feature** | Explicitly requested: the `update` field, the OTA flow, the web UI. Cite the D or DEC number. |

A behaviour that fits none of the three is out of scope and must be escalated, not
implemented. **A test that pins a behaviour with no justification is itself a
defect** — it freezes an unreviewed decision into the suite.

### Accepted production behaviour — not defects, do not "fix"

| # | Behaviour | Where in the original | Ruling |
|---|---|---|---|
| P1 | **No debounce on the magnet trigger.** `lastDebounceTime` (`main.cpp:311`) and `timerStarted` (`:312`) are declared and never read; the edge test at `:322` is bare; the 1 ms `vTaskDelay` at `:340` is the poll period, not a filter. A bouncing reed fires the stop on the first LOW, and the bounce back to HIGH takes the release branch and blocks for a whole cooldown. | `main.cpp:305-345` | bigboss: **leave as is**. The blind window is accepted production behaviour. Do not add a filter, a config field or a TODO. Recorded here so it is never logged as a bug found in testing. |
| P2 | **Cooldown blocks rather than filters.** The pin is not read at all during the window; a magnet pass inside it is missed, not suppressed. | `main.cpp:336` | matches-original. Pinned by T26b. |
| P3 | **The initial cooldown is load-bearing.** The magnet that fired the ext0 wake is still over the reed when the task starts with a remembered level of HIGH. | `main.cpp:314` | matches-original. Pinned by T22. |
| P4 | **The idle-stopped acquisition is discarded unsent.** The flag is checked before the transmit block. | `main.cpp:169`, sleeps out at `:174` | matches-original. |
| P5 | **`max_acq` counts attempts, not successful uploads.** The counter increments on entry to the collect, before anything downstream can fail. Counting successes would hang a device at full draw with the server unreachable. | `ICM42688P.cpp:363` | matches-original, and structurally enforced: `AcquisitionService` never learns whether a transmit happened. |
| P6 | **`max_acq == 0` ends the cycle without collecting.** | falls out of `loopCounter > nSamples` | matches-original. The web form refuses 0; the firmware still honours it. |
| P7 | **The cold-boot 10 s timer sleep is dead code.** `wakeup_stage` is initialised to 1 and nothing ever assigns 0. | `main.cpp:19,139` | DEC-10 amended: **not restored**. |
| P8 | **Server config is not persisted across deep sleep.** `IIM.response` is plain RAM; every timer wake starts from the compiled defaults until the next server contact. | `ICM42688P.h:257-264` | matches-original. NOTE: the refactor holds `serverConfig` in `RTC_DATA_ATTR` (`app.cpp:12`), so it now **survives** a deep-sleep wake. That is a deliberate divergence and needs a DEC-0 justification on the record — see [gaps](#4-what-worries-me-most). |

---

## 1. Acceptance criteria — D1 duty cycle

### 1.1 Boot and state machine

| # | Criterion (falsifiable) | Status | Test / note |
|---|---|---|---|
| A1 | Any reset that is **not** a deep-sleep wake lands in `ARM_TRIGGER`, never mid-cycle. | host | `test_unexpected_reset_lands_in_arm_trigger` |
| A2 | Every `BeltStage` yields an action; no stage falls through. `BELT_ACTION_INVALID` is never returned. | host | `test_every_stage_is_handled` |
| A3 | `RTC_DATA_ATTR` is retained across a deep-sleep wake and reloaded from flash on every other reset. | **HW** | H6. The whole reset-safety argument rests on this and it came from documentation, not observation. |
| A4 | Cold boot arms ext0 and sleeps — no 10 s timer stage. | host | `test_cold_boot_arms_trigger_and_sleeps` (DEC-10 amended) |
| A5 | A magnet wake enters `CYCLE`. | host / **HW** | `test_trigger_wake_enters_cycle` proves the decision; that ext0 on GPIO33 LOW actually wakes the chip is H1. |

### 1.2 Acquisition and trigger

| # | Criterion | Status | Test / note |
|---|---|---|---|
| A6 | Radio off **then** CPU to 10 MHz before acquiring. | host | `test_entering_acquisition_mode_turns_the_radio_off_then_drops_to_10mhz`, `..._even_when_not_connected` |
| A7 | The acquisition ends when the belt trigger fires. | host | `test_collect_stops_when_trigger_requests_stop` |
| A8 | The cooldown lasts `trigger_cooldown_sec` — the configured value, not a constant. | host | T25 `test_cooldown_uses_configured_seconds_not_a_constant` |
| A9 | Levels are not observed during the cooldown (blocking, not filtering). | host | T26b `test_levels_are_not_observed_during_cooldown` + `mutant_cooldown_samples_through` |
| A10 | The magnet that caused the wake does not end the first acquisition. | host | T22 `test_stop_is_not_requested_during_the_initial_cooldown` |
| A11 | **The device stays awake between acquisitions** — no deep sleep inside a cycle. | host | `test_cycle_stays_in_cycle_after_transmit_when_acquisitions_remain`. This is the core of D1. |
| A12 | The counter advances once per iteration whatever the transmit did. | host | `test_acquisition_count_increments_even_when_the_transmit_fails`, `test_acquisition_count_is_not_advanced_by_steps` |
| A13 | The cycle ends after `max_acq` iterations. | host | `test_cycle_sleeps_by_timer_after_max_acquisitions` |
| A14 | `max_acq == 0` ends the cycle without collecting. | host | `test_max_acquisitions_of_zero_ends_the_cycle_immediately` |
| A15 | `idle_timeout_min` is **minutes**. 20 min without a trigger ends the cycle. | host | T28/T29 + `mutant_idle_timeout_in_seconds` |
| A16 | An idle-stopped acquisition is discarded unsent and the ring is **not** cleared. | struct | `app.cpp:110-118`. The decision is host-visible (`ACQ_STOPPED_BY_IDLE`); that the caller then returns without transmitting is Arduino-side. Round 9. |
| A17 | The counter resets on **both** cycle-end paths. | host | `test_acquisition_count_resets_when_the_cycle_ends_by_either_path` |

### 1.3 Sampling, frame and buffer

| # | Criterion | Status | Test / note |
|---|---|---|---|
| A18 | A stored frame is 18 B: `ts(4 LE) accX gyroX accY gyroY accZ gyroZ temp`, each int16 LE. | host | `test_stored_frame_is_18_bytes_in_wire_order` |
| A19 | The timestamp is the one sampled **before** the SPI burst. | host | `test_frame_timestamp_is_the_one_passed_to_step`. The interface makes the late-timestamp regression inexpressible. |
| A20 | `DATA_RDY` clear → nothing stored. | host | `test_not_ready_samples_are_not_stored` + `mutant_store_when_not_ready` |
| A21 | Allocation is exactly 700000 B; capacity 38888 frames; usable 699984 B. | host | `test_allocation_requests_exactly_the_production_buffer_size` + 3 `static_assert`s in `board.h:65-69` |
| A22 | **(DEC-1)** Every payload length is a whole number of frames: `bytes % 18 == 0`. | **open** | Guaranteed by frame-addressed capacity, but no test asserts it. Add `bytesStored() % SAMPLE_SIZE_BYTES == 0` after a wrap, and mutation M3. |
| A23 | A failed allocation is fatal, the ring reports zero capacity, and no sample is ever written. | host / **HW** | `test_allocation_failure_calls_fatal`, `test_failed_allocation_yields_a_ring_that_reports_no_capacity`, `test_no_sample_is_ever_written_when_allocation_failed` + `mutant_skip_allocation_check`. A *real* PSRAM failure is H4. |
| A24 | Not wrapped → one range from index 0 (byte-identical to production). Wrapped → two ranges, tail first. | host | `test_plan_is_one_range_from_zero_when_not_wrapped`, `test_plan_is_two_ranges_starting_at_tail_when_wrapped`, `test_append_overwrites_oldest_frame_when_full`, `test_bytes_stored_never_exceeds_capacity` |
| A25 | The **live** transmit sends both ranges when wrapped. | **open** | `app.cpp:128-131` sends `plan.first` only. R8, round 7 (T45). Today a wrapped ring silently truncates. |

### 1.4 Transmission

| # | Criterion | Status | Test / note |
|---|---|---|---|
| A26 | Uplink per connection: `[4 B uint32 LE total][N × 18 B][2 B uint16 LE battery_mv]`. | **open** (firmware side) | `TcpClient` lives in `src/`, not host-compiled. Pinned only from the reader end by `backend/tests/test_recv_exact.py` + `test_tcp_server.py`. Round 9 should move the framing into `lib/`. |
| A27 | Battery = `analogRead(36)` mapped `0..4095 → 0..19803` mV. | **HW** | H11. Verified by reading only (disclosed by gomi); the two-line pin lands in round 7. |
| A28 | Only a **successful** send resets the ring; a failed transmit carries its data forward. | struct | `app.cpp:131-139` (matches `main.cpp:264-265`). T47b, round 7. |
| A29 | The radio goes off at the end of every iteration, on every path. | struct | `app.cpp:144-151` says so explicitly: *"STRUCTURAL, not tested"*. R7 closed by construction; round 9 extracts it. |

### 1.5 Config response

| # | Criterion | Status | Test / note |
|---|---|---|---|
| A30 | The response is exactly 10 B: offsets 0/2/4/6/8 = `sleep_min, idle_min, max_acq, cooldown_sec, update`, uint16 LE. | host | `test_server_config_is_10_bytes`, `test_server_config_field_offsets_are_0_2_4_6_8`, `test_parse_config_from_golden_le_blob_240_20_5_5_1` |
| A31 | A frame shorter than 10 B leaves the config untouched — no half-reconfiguration. | host, **not live** | `test_parse_config_rejects_frame_shorter_than_10_bytes`. `TcpClient::sendData` still `readBytes` straight into the struct; `parse_server_config` is not on the live path. T46, round 7. |
| A32 | Defaults before first contact: 240 / 20 / 5 / 5 / 0. | host | `test_defaults_are_240_20_5_5_0` |
| A33 | A new `cooldown_sec` takes effect on the next cooldown. | host | T25 + `app.cpp:135` |
| A34 | The legacy `pyFiles/win_server.py` (`struct.pack('<HHHHH', ...)`) stays wire-compatible. | host | Same 10-byte golden blob. Worth an explicit round-trip test against the literal original packing. |

### 1.6 Sleep

| # | Criterion | Status | Test / note |
|---|---|---|---|
| A35 | Before **either** sleep: radio off, IMU asleep, then arm and go. | host | `test_timer_sleep_turns_the_radio_off_and_sleeps_the_imu_first`, `test_trigger_sleep_...` + `mutant_imu_stays_awake_on_trigger_sleep` |
| A36 | Timer sleep duration is `sleep_min × 60 000 000 µs`. | host | `test_timer_sleep_duration_is_sleep_min_times_60_million_microseconds` |
| A37 | On the timer wake the device arms ext0 and sleeps waiting for the magnet. | host | `test_timer_wake_arms_trigger` |
| A38 | Deep-sleep current draw is low enough for the battery budget. | **HW** | H3. Nothing on the host says anything about amps. |

### 1.7 OTA — round 8, entirely open

| # | Criterion | Status | Test / note |
|---|---|---|---|
| A39 | `update=1` → `Preferences("config").putBool("update", true)` → `ESP.restart()`. | **open** | `app.cpp:55` hardwires `otaFlagSet = false`. Decision table ready (`test_server_update_flag_routes_to_restart`, `test_ota_flag_at_boot_routes_to_ota_before_any_acquisition`); nothing in `src/` acts on it. |
| A40 | The flag survives `ESP.restart()` via NVS. | **open / HW** | H7. Today a claim, not a test. Tooling question Q1. |
| A41 | On the next boot the flag is **cleared first**, then the AP comes up: `WIFI_AP`, SSID `Update driver - <MAC>`, password `12345678`. Cleared-first means a crashed OTA cannot loop. | **open / HW** | H8. matches-original `main.cpp:74-99`. |
| A42 | The AP serves `updatePage` on port 80 with `/update`, `/version`, `/restartESP`; 5 min timeout then restart. | **open / HW** | H8. matches-original `main.cpp:347-405`. |
| A43 | Arming is **one-shot**: exactly one device gets `update=1`, and a failed send gives the arming back. | host (backend) | `AppState.take_config_for_send` / `rearm_ota`, claimed under a single lock. In `backend/tests/`. |
| A44 | The config form never reads or writes the OTA flag (DEC-5). | host (backend) | `POST /ota` is its own route (`web/server.py:62`). |

### 1.8 Cross-cutting

| # | Criterion | Status | Test / note |
|---|---|---|---|
| A45 | **(DEC-2)** No real SSID or password anywhere in the tree, and a missing secret **fails the embedded build loudly**. | **open** | Not implemented. `src/config/server_config.cpp` still holds `"your_ssid"`/`"your_password"`; no `secrets.h`, no `extra_configs` in `platformio.ini`, no `.gitignore` line. A build with no secret currently produces a **mute device**, which is exactly what DEC-2 forbids. Public repo. |
| A46 | The native build needs no secret. | host | `env:native` does not compile `src/`. Holds today by construction; will need re-checking when A45 lands. |
| A47 | Nothing under `lib/` includes `Arduino.h`, `SPI.h`, `WiFi.h` or `esp_*`. | host | Enforced by `env:native` failing to link. |

---

## 2. Hardware-in-the-loop

Nothing below can be proven on the host. Each row is a bench or belt procedure with
the observation that would mean a regression.

| # | What | Procedure | Regression signal |
|---|---|---|---|
| **H1** | **ext0 wake on GPIO33 LOW** | Flash, let it reach `ARM_TRIGGER` and sleep. Short GPIO33 to GND with a wire, then release. Watch the serial line for the boot banner. Repeat 10×. | No boot on the short; or a boot with no short (floating pin — `INPUT_PULLUP` is disabled in deep sleep, so an external pull-up may be required); or a wake that lands anywhere but `CYCLE`. |
| **H2** | **Reed switch vs belt speed** | Scope GPIO33 with the real magnet on the moving belt at production speed. Measure LOW pulse width and bounce train. | Pulse width **< ~2 ms**: the 1 ms poll can miss it and the acquisition never ends (falls through to the 20 min idle timeout — the symptom is "acquisitions are 20 minutes long"). Also record the bounce train for the record — per P1 we do not filter it, but we must know its width. |
| **H3** | **Deep-sleep current** | µA meter in series with the battery. Sample in `ARM_TRIGGER` sleep and in timer sleep. Note the brownout detector is disabled (`app.cpp:21`) and PSRAM is populated. | Above the battery budget for 240 min × the duty cycle. Compare against the **original firmware on the same board** — DEC-0 makes the original the reference, not a datasheet figure. |
| **H4** | **Real `ps_malloc(700000)`** | Boot a pico32 with PSRAM; confirm the ring reports 38888 frames. Then boot a board with PSRAM disabled (drop `-DBOARD_HAS_PSRAM`) and confirm the halt. | Allocation succeeds but capacity ≠ 38888; or a failed allocation that does **not** halt (that is R1 restored — and per `mutant_skip_allocation_check` the failure mode is silence, not a crash). |
| **H5** | **SPI sampling at ODR 50 Hz** | Acquire for a known wall-clock interval with the trigger held off. Count frames received server-side and diff consecutive frame timestamps. | Rate materially off 50 Hz; timestamp deltas not ≈20 ms; or gaps that indicate `DATA_RDY` is being polled faster than the ODR and frames are being dropped or duplicated. |
| **H6** | **`RTC_DATA_ATTR` semantics (R9)** | Print `stage` at boot. (a) timer deep sleep → wake: expect retained. (b) `ESP.restart()`: expect the initial value. (c) power cycle: expect the initial value. (d) EN-pin reset: expect the initial value. | Any case where a **non-deep-sleep reset retains the stage** breaks A1 — the device could resume mid-cycle after a crash. This is the single most load-bearing unverified assumption on the branch. |
| **H7** | **NVS flag across `ESP.restart()`** | Arm OTA from the web UI, let a device transmit, watch it restart, confirm the AP appears. Then power-cycle mid-window and confirm it does **not** come back into OTA. | The flag not surviving the restart (no AP — feature dead) or surviving past its clear (a permanent OTA loop — device unreachable in the field). Both are field-fatal and neither is host-testable. |
| **H8** | **SoftAP OTA upload** | Join `Update driver - <MAC>` / `12345678`. `GET /version`, upload a build via `/update`, confirm reboot into the new version. Separately, join and do nothing for 5 min and confirm the restart. | Upload fails or bricks; `/version` unchanged after a reported success; the 5 min timeout does not fire (a device that never leaves the AP is off the belt permanently). |
| **H9** | **Brownout during WiFi TX on battery** | Full cycle on a battery at low state of charge, radio at 240 MHz transmitting ~700 kB. | Reset or truncated transmission. The brownout detector is **disabled**, so the failure mode is corruption rather than a clean reset — check the server-side byte count against the header. |
| **H10** | **Static IP + 5 s connect on the plant AP** | Bring the device up against the real AP with the real credentials. Time `WiFi.begin` → `WL_CONNECTED`. | Above 5 s: the connect times out, no transmit happens, and — per P5 — the counter still advances, so the whole wake burns its 5 acquisitions on nothing. Silent in every host test. |
| **H11** | **Battery ADC calibration** | Bench supply across the divider at several points; compare the reported mV against a meter. | Reported mV diverging from the original firmware's reading on the same board. DEC-0: the original's mapping is the reference, including its error. |
| **H12** | **Full duty cycle on a belt** | One complete round: magnet wake → 5 acquisitions with 5 s cooldowns → 5 connections server-side → timer sleep → ext0 wake. | Anything other than exactly 5 connections with monotonic timestamps; a 6th; a cycle that ends early; a sleep that is not ~240 min. This is the end-to-end acceptance run for D1 and nothing short of a belt proves it. |

**Not on this list on purpose:** `PowerManager` ordering (R5/R7) used to be here and is
not any more. It looked hardware-only and was in fact a design problem — once the
collaborators record their calls, "radio off *before* the IMU sleeps" is an ordinary
host assertion. Worth remembering the next time something is called untestable: check
whether it is untestable or merely unseamed.

---

## 3. What the mutation harness does not defend

Six mutations exist and all six behave (round 6: every named catcher dead, every
named survivor alive). The harness is the strongest quality artefact on this branch.
The gaps are about **coverage**, not correctness.

### 3.1 The structural gap

Mutations only reach `lib/`. `env:native` does not compile `src/`, so **nothing in
`src/` has either a test or a mutation** — `app.cpp`'s cycle wiring, `TcpClient`'s
framing, `WiFiManager`, the battery read. `app.cpp:149-150` states this plainly. It
is the right call for now and round 9 is the answer, but until then A16, A26, A28 and
A29 rest on reading the code.

### 3.2 Behaviours with no mutation, ranked

| # | Proposed mutation | Guards | Why it matters | Predicted catchers — **name these before running** |
|---|---|---|---|---|
| **M1** | `MUTANT_MAX_ACQ_OFF_BY_ONE` — `belt_cycle` compares `>` where it compares `>=` (or the reverse). | A13 | "5 acquisitions" is the number bigboss specified and the one the operator counts on the history page. An off-by-one gives 4 or 6 and every other test stays green. The original's own `loopCounter > nSamples` is exactly the kind of boundary that survives a refactor wrong. | `test_cycle_sleeps_by_timer_after_max_acquisitions`, `test_max_acquisitions_of_zero_ends_the_cycle_immediately`. If **only** the zero test dies, the boundary at 5 is not pinned. |
| **M2** | `MUTANT_FRAME_FIELD_ORDER` — swap `gyro` and `accel` within an axis when the frame is assembled. | A18 | The 18-byte layout is the contract with the CSV corpus *and* with `pyFiles/win_server.py`. A silent swap produces a file that parses cleanly, plots plausibly, and is wrong. Nothing downstream would ever reject it. | `test_stored_frame_is_18_bytes_in_wire_order` only. If it survives, that test is checking length rather than order. |
| **M3** | `MUTANT_RING_WRAP_BY_BYTES` — advance the tail by one byte instead of one frame on overwrite. | A22, A24 | This is DEC-1's whole point. A byte-misaligned wrap makes every subsequent frame garbage while lengths and counts stay plausible — the exact failure the frame-addressed design was chosen to prevent. Prevention with no detector is a design comment. | `test_plan_is_two_ranges_starting_at_tail_when_wrapped`, `test_append_overwrites_oldest_frame_when_full`. Add the `% 18 == 0` assertion from A22 first — I expect it is the only thing that reliably dies. |
| **M4** | `MUTANT_PARSE_ACCEPTS_SHORT_FRAME` — `parse_server_config` fills what it has and returns true. | A31 | Half a config is worse than none: a truncated response could set `sleep_min` from two stray bytes and put a device to sleep for an arbitrary time. The guard exists; nothing has shown it can fire. | `test_parse_config_rejects_frame_shorter_than_10_bytes`. A near-certain kill — which per the harness's own rules makes it a formality. Worth wiring anyway *because A31 is not on the live path*: the mutation documents that the guard is real while the wiring is still missing. |
| **M5** | `MUTANT_ENDCYCLE_RESETS_ONLY_IDLE_PATH` — reset the counter on the idle path only. | A17 | The original resets on both (`ICM42688P.cpp:438` and `:445`). Resetting on one leaks into the next cycle, and the symptom is a device that does 5 acquisitions on one wake and 1 on the next. | `test_acquisition_count_resets_when_the_cycle_ends_by_either_path`. Named after the behaviour, so if it survives the test is checking one path under a two-path name. |

M1 and M3 are the two I would actually spend the round on. M2 is cheap and protects
the only contract with a decade of historical data behind it.

### 3.3 One mutation I do **not** recommend

`MUTANT_COUNT_SUCCESSFUL_TRANSMITS_ONLY` looks like the obvious guard for P5/A12, and
it cannot be written: `AcquisitionService` has no way to learn whether a transmit
happened. That is the point of the design, and a mutation that has to add a
collaborator to express itself is testing a different program. The guarantee is
structural and the comment on `BeltInputs::acquisitionsDone` is the right artefact.

### 3.4 Process note

The rule `#ifdefs == envs == README rows` earned its place this session: I read the
tree in the window between `power_manager.cpp` and `platformio.ini` being written and
saw a mutation with no env. It closed itself minutes later, but the check is a
one-line grep and it catches a class the other two rules cannot, because both of those
assume a run happened and produced output to be suspicious of. This one produces
nothing.

```
grep -rho 'MUTANT_[A-Z_]*' lib/ | sort -u          # ifdefs
grep -o 'MUTANT_[A-Z_]*' platformio.ini | sort -u  # envs
grep -o 'MUTANT_[A-Z_]*' test/README.md | sort -u  # rows
```

---

## 4. What worries me most

Ranked, and each is a question for lave rather than a finding.

1. **A45 / DEC-2 is not implemented, and the repo is public.** Placeholder
   credentials that build cleanly are the failure mode DEC-2 named explicitly: the
   build succeeds and the device is mute. This is the only open item with a
   consequence outside the codebase.
2. **A3 / H6 is an assumption, not an observation.** `BELT_INITIAL_STAGE` being the
   safe state is the argument for reset safety, and it holds only if a non-deep-sleep
   reset really does reload RTC memory. Everything in §1.1 is downstream of a fact
   nobody has watched happen.
3. **P8: `serverConfig` moved from RAM to `RTC_DATA_ATTR`.** In the original, config
   does not survive deep sleep — every timer wake starts from the compiled defaults.
   In the refactor it does. That is arguably better and it is a **behaviour change**,
   so under DEC-0 it needs a justification on the record or a revert. Concretely: a
   device that once received `sleep_min = 1` keeps it forever across wakes, where
   production would have reverted to 240 on the next boot.
4. **A25 — a wrapped ring truncates silently today.** ~777 s of belt at 50 Hz before
   the first wrap, so a belt that runs longer than 13 minutes between magnet passes
   hits it. The idle timeout is 20 minutes, so the window is real, not theoretical.
   Round 7 closes it; until it does, the failure is silent on both ends.
5. **A26 — the uplink framing has no firmware-side test at all.** It is pinned only
   from the reader end. The two sides could drift and the backend suite would stay
   green.

---

## 5. Coverage matrix — behaviour → level → owner

| Area | Host (native/Unity) | Host (pytest) | Mutation | HW | Owner |
|---|---|---|---|---|---|
| Belt state machine / D1 | A1–A5, A11, A13, A14, A37 | — | M1 *(proposed)* | H12 | gomi |
| Trigger + cooldown | A7–A10 | — | `cooldown_samples_through` | H1, H2 | gomi |
| Acquisition + idle timeout | A12, A15, A17, A20 | — | `idle_timeout_in_seconds`, `store_when_not_ready`, M5 *(proposed)* | H5 | gomi |
| Ring buffer / DEC-1 | A21, A22, A24 | — | `skip_allocation_check`, M3 *(proposed)* | H4 | gomi |
| Frame layout | A18, A19 | `test_packet.py` | M2 *(proposed)* | H5 | gomi / sama |
| Uplink framing | — | `test_recv_exact.py`, `test_tcp_server.py` | — | H9, H10 | sama (reader), gomi (writer, round 9) |
| Config response (10 B) | A30–A34 | `test_packet.py` | M4 *(proposed)* | — | gomi / sama |
| Power ordering | A6, A35, A36 | — | `imu_stays_awake_on_trigger_sleep` | H3 | gomi |
| Battery | — | — | — | H11 | gomi |
| OTA arming (server) | — | `test_app_state.py`, `test_web_server.py` | — | — | sama |
| OTA device (SoftAP) | A39 *(decision only)* | — | — | H7, H8 | gomi, round 8 |
| Secrets / DEC-2 | A45, A46 | — | — | — | gomi — **open** |
| Reset safety | A1, A2 | — | — | H6 | gomi |

Owners follow the ownership map: gomi = `src/ lib/ include/ platformio.ini test/`,
sama = `backend/**`, keri = `docs/qa/**`. This document assigns no work; it records
where each behaviour is proven and where it is not.
