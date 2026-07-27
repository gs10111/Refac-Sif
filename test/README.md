# Firmware tests

Host tests, no hardware needed:

```
pio test -e native
```

On-target tests (for the checks that need real hardware — ext0 wake, PSRAM
allocation failure, SPI timing):

```
pio test -e pico32
```

Each suite is its own directory (`test_protocol/`, `test_ringbuffer/`, ...) —
PlatformIO builds and links one binary per directory. Every suite file carries
both an `ARDUINO` `setup()/loop()` entry point and a plain `main()`, so the same
source runs in either environment.

## Layout rule

Only Arduino-free code is testable on the host. Pure modules live under `lib/`
and must not include `Arduino.h`, `SPI.h`, `WiFi.h` or `esp_*`. `src/` is the
Arduino wiring layer and is not compiled by `env:native`.

## Mutations

```
pio test -e mutant_cooldown_samples_through
pio test -e mutant_idle_timeout_in_seconds
pio test -e mutant_store_when_not_ready
pio test -e mutant_skip_allocation_check
pio test -e mutant_imu_stays_awake_on_trigger_sleep
pio test -e mutant_send_from_index_zero
pio test -e mutant_clear_ota_flag_before_ap
pio test -e mutant_disarm_writes_unconditionally
```

A green suite proves the tests *can* pass. It does not prove they can *detect*
anything — a test whose subject is an absence ("this does not happen") is
satisfied by an implementation that does nothing at all. The only way to tell the
two apart is to run the tests against an implementation that is known to be wrong.

Each mutant env builds the same tests against deliberately broken code, guarded by
exactly one `-DMUTANT_*` flag. **Failures are the pass condition.** If a mutation
runs green, the tests that were supposed to catch it are documenting an intention
rather than enforcing a behaviour.

| Env | Flag | Breaks | Caught by | Survived by |
|---|---|---|---|---|
| `mutant_cooldown_samples_through` | `MUTANT_COOLDOWN_SAMPLES_THROUGH` | `BeltTrigger::poll` tracks the pin level during the cooldown and suppresses the event, instead of not reading the pin at all | `test_belt_trigger` T23, T26, T26b | T22, T24, T25 — none change level inside the window |
| `mutant_idle_timeout_in_seconds` | `MUTANT_IDLE_TIMEOUT_IN_SECONDS` | `AcquisitionService` treats `idle_timeout_min` as seconds | `test_acquisition` T29, at its 20001 ms probe | **T28** — a 20-second timeout also stops at 1200001 ms, so T28 pins only the endpoint |
| `mutant_store_when_not_ready` | `MUTANT_STORE_WHEN_NOT_READY` | frames are stored with `DATA_RDY` clear, filling the buffer at the polling rate instead of the ODR | `test_acquisition` T32 | T33, T33b — they set the fake ready, so the mutation is invisible to them |
| `mutant_clear_ota_flag_before_ap` | `MUTANT_CLEAR_OTA_FLAG_BEFORE_AP` | the OTA flag is spent before the access point exists — production's order | `test_ota` T50 (call sequence), T51, T52 | T54, T55, T56, T57 — none reach that function with the flag set |
| `mutant_disarm_writes_unconditionally` | `MUTANT_DISARM_WRITES_UNCONDITIONALLY` | the disarm writes on every config receipt instead of only on the transition — right state, one NVS write per connection forever | `test_ota` T56 | **T55, T57** — the flag ends correct either way; the cost is flash endurance, not behaviour |
| `mutant_send_from_index_zero` | `MUTANT_SEND_FROM_INDEX_ZERO` | the uploader sends only the first range, from index 0, ignoring `tail` — R8 verbatim | `test_transmit` T45 | **every other test in the suite** — they use an unwrapped ring, where the two implementations agree byte for byte. That is why R8 survived review: the wrong code is correct for the first thirteen minutes |
| `mutant_imu_stays_awake_on_trigger_sleep` | `MUTANT_IMU_STAYS_AWAKE_ON_TRIGGER_SLEEP` | `PowerManager` skips `imu.sleep()` on the ext0 path only — R5 on the sleep that lasts hours | `test_power` T38 | **T37**, deliberately: breaking one path proves the two are pinned independently, which killing both would not |
| `mutant_skip_allocation_check` | `MUTANT_SKIP_ALLOCATION_CHECK` | `make_sample_ring` does not check the allocation, building a full-capacity ring over a null pointer — R1 restored | `test_sample_store` T35, T36b | **T36** — nothing is written anyway, because `RingBuffer` refuses a null storage pointer. The defence is in the ring, not in the check |

Each flag is documented at the `#ifdef` that implements it. Rules:

- **Every flag needs an env and a row, and the three counts must match.** A
  mutation that exists only as an `#ifdef` is never run by anyone: `pio` errors on
  an unknown env, so nothing reports anything at all, and the behaviour it was
  written to defend is left resting on an assertion. Neither of the two rules below
  catches this — the mutation is not weak and the flag is not unwired, the env
  simply is not there. One grep settles it:

  ```
  grep -rho '^#ifdef MUTANT_[A-Z_]*' lib src | sort -u | wc -l   # flags in source
  grep -c '^\[env:mutant_' platformio.ini                        # envs
  grep -c '^| `mutant_' test/README.md                           # table rows
  ```

  Three equal numbers, or something is missing. Run it before claiming a mutation
  round is done.
- **Name which tests must die, before the run.** A predicted *count* is not a
  self-check: "expect 3 failed" is satisfied by the wrong three. That is precisely
  what happened here — three failures arrived from an older mutation while two new
  flags sat undefined, and the total looked entirely plausible. Naming the specific
  tests makes the run self-checking: if the named ones live and different ones die,
  the mutation never executed. If the named ones live and nothing else dies, the
  tests are weak. A count cannot tell those two apart, and they call for opposite
  responses.
- **The first run of a new mutation must FAIL.** An all-green mutant env does not
  mean the code is safe — it almost always means the flag is not wired. A harness
  whose flag is never defined reports success in exactly the same shape as one that
  ran, and the total looks entirely plausible. This has already happened once here:
  two `#ifdef`s were added and neither flag reached `platformio.ini`, so the run
  compiled the clean code and passed. Checking the count would not have caught it;
  only reading the build flags did. One env per mutation exists so that this failure
  shows up as *that env* going green, which is loud and specific.
- One env per mutation, exactly one `-DMUTANT_*` flag each. Never one env carrying
  several — conflated results hide which mutation a failure belongs to.
- `env:native` and `env:pico32` never compile a mutant branch.
- Choose and name the mutation **before** running it. One picked after seeing which
  tests are weak proves nothing.
- Don't edit a test to catch a mutation you only predicted. Get the observed output
  first — otherwise you have tuned the test to your imagination.
- Prefer a mutation that could plausibly **survive**. One the suite obviously kills
  is a formality; the useful one is the one you are unsure about. Feeling confident
  it will be caught is a signal to pick a subtler one.
- Record which tests catch each mutation *and which do not*. An assertion that
  survives a mutation is not dead weight — it is usually the detector for a
  different one — but the table must not imply every assertion earns its place
  against every flag.

## Checks a test cannot make

Some properties are facts about a declaration rather than about any behaviour a
fake can observe. Writing a test for one of those produces a test that passes
whichever way the declaration goes, which is worse than admitting the gap. Use a
grep, and keep it here where it will be run.

```
grep -c 'RTC_DATA_ATTR' src/app/app.cpp                    # must be exactly 1
grep -rn 'your_ssid\|your_password' src include lib       # must return nothing
git check-ignore include/secrets.h                         # must succeed
```

Only the belt stage belongs in RTC memory. The server config must NOT: production
holds it in an ordinary member (`ICM42688P.h:257-264`), so a timer wake starts from
the compiled defaults and the server re-states it on the next transmission. A device
handed a bad `sleep_min` then self-heals on its next wake, where persisting it would
put a device that took `sleep_min = 1440` permanently out of reach of the fix.

The credential checks guard DEC-2. This repository is public; the real SSID and
password live only in `include/secrets.h`, which is git-ignored, and a build without
it fails with an `#error` rather than compiling a placeholder. The placeholder is
the failure being prevented: it builds, links, flashes and boots, and produces a
device that acquires perfectly and can never reach the server.

Two other guarantees are currently structural rather than tested, both because the
cycle lives in Arduino code a host build cannot reach. Round 9's extraction is where
they stop being so:

- `runCycleIteration` reaches `_power.enterAcquisitionMode()` on every path
  (`app.cpp`, one unconditional call outside every branch — this is R7's shape).
- `App::begin` calls `_imu.wake()` exactly once per boot rather than per acquisition.

## Why a red stub round is not coverage

The stub round proves a test is **wired to the code**. Only a mutation proves it can
**detect** anything. Those are different guarantees and they are easy to conflate,
never more so than when every test in a suite goes red at once.

Consider a pair of complementary absences: T28 "stops after the idle timeout" and
T29 "does not stop before it". The inert stub has to return *some* constant, and
whichever one it returns carries one of the pair for free — return `ACQ_RUNNING` and
T29 passes on every assertion, return `ACQ_STOPPED_BY_IDLE` and T28 does. No single
stub can put both under pressure. Ten tests going red therefore does not mean ten
tests can detect; it means ten tests are connected to the thing they name.

The same applies to any test whose subject is silence — "this is not stored", "the
radio is off", "nothing happens during the window". A stub that does nothing
satisfies them completely.

## Assert the size before dereferencing

Any test that reads through a pointer from a `ReadPlan`, a buffer or a fake must
assert its length first:

```c
TEST_ASSERT_EQUAL_UINT32(SAMPLE_SIZE_BYTES, plan.totalBytes());  // guard
TEST_ASSERT_EQUAL_UINT8(0xDD, plan.first.ptr[0]);
```

An empty plan carries a null range. A test that segfaults does not fail — it takes
down every remaining result in the same binary, and the suite reports the loss as
silence.

## Ground rules

Tests come first — a failing test before the production code that satisfies it.

Where a test pins behaviour that matches the original production firmware rather
than behaviour we think is better, the comment says so. The original is in
production and works, defects included; matching it is the tie-breaker.
