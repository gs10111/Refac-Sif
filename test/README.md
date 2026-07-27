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
pio test -e mutant_max_acq_off_by_one
pio test -e mutant_radio_off_only_on_success
pio test -e mutant_frame_capacity_rounds_up
pio test -e mutant_battery_read_before_upload
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
| `mutant_radio_off_only_on_success` | `MUTANT_RADIO_OFF_ONLY_ON_SUCCESS` | the radio is turned off only inside the successful-connect branch — R7 as it shipped | `test_cycle` A21, A23 | **A20** — on the happy path both versions turn it off, which is why reading the function and believing it let R7 ship |
| `mutant_frame_capacity_rounds_up` | `MUTANT_FRAME_CAPACITY_ROUNDS_UP` | the ring claims 38889 frames spanning 700002 bytes over a 700000-byte allocation — two bytes past the end, on every wrap | `test_ringbuffer` `..._never_spans_more_bytes_than_it_was_given` and T06; `test_sample_store` T34 | every test that constructs a ring with an explicit frame count — they never call the function |
| `mutant_max_acq_off_by_one` | `MUTANT_MAX_ACQ_OFF_BY_ONE` | `belt_next` uses `>` for `>=`, so a cycle runs one acquisition more than configured | `test_belt_cycle` T15 **and T31d** | T14 — it only walks `done` from 1 to max-1, where both operators agree |
| `mutant_battery_read_before_upload` | `MUTANT_BATTERY_READ_BEFORE_UPLOAD` | the ADC is sampled before any bytes go out, radio idle, instead of after the payload as production does at `main.cpp:259` | `test_transmit` A27b, which pins the byte count at the moment of the read | **every other test** — they assert the value on the wire, identical either way |
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
  grep -rhoE '^#ifdef MUTANT_[A-Z][A-Z_]*' lib src | sort -u | wc -l   # flags in source
  grep -cE '^\[env:mutant_' platformio.ini                            # envs
  grep -cE '^\| `mutant_' test/README.md                              # table rows
  ```

  Three equal numbers, or something is missing. Better, because it catches BOTH
  directions — a flag with no env, and an env that compiles clean code and reports
  success in the same shape as one that ran:

  ```
  comm -3 <(grep -rhoE 'MUTANT_[A-Z][A-Z_]*' lib/ | sort -u) \
          <(grep -oE  'MUTANT_[A-Z][A-Z_]*' platformio.ini | sort -u)
  ```

  Empty output is the pass condition. Require a letter after the prefix: a looser
  `MUTANT_[A-Z_]*` also matches the bare `MUTANT_` inside prose like "-DMUTANT_*
  flags", and reports a phantom asymmetry from a comment.

  Run these against a tree nobody is editing. A count taken mid-edit measures the
  editor, not the code.
- **A mutation's name must match what it does.** One here advertised "wraps by
  bytes" and actually froze the head at zero: `(head * 18 + 1) / 18` advances one
  BYTE, and the truncating divide means the frame index never moves. Five tests
  died and the run looked like a strong result — the three unnamed deaths are the
  only reason anyone went looking. A mutation whose behaviour does not match its
  row makes the report claim coverage that was never verified, and it fails
  *convincingly*.
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

## A check that can pass by breaking

Every invariant check below reports a *pass value*. Ask what happens when the check
itself is wrong, because for most of them a broken check produces the pass value:

- `comm -3 ...` passes on **empty**. A pattern that matches nothing is also empty.
- The three counts pass on **equality**. A pattern family that breaks the same way in
  all three gives `0 == 0 == 0`, which reads as pass on a tree with no mutations
  wired at all.

The second is the dangerous one, and it is dangerous *because the three checks are
correlated*: they share a pattern family and get edited together — all three were
edited here in one commit. Independent checks degrade to a mismatch, which is
visible. Correlated ones degrade to agreement, which is not. Correlation is what
turns three measurements back into one.

This has already happened, twice on the same line. The README row count was attempted
three times by two people and returned **0, 0, 12** — two different bugs (a pattern
matching a text shape the table does not have; a backtick inside a double-quoted
`$( )` opening a command substitution and eating the pattern), identical output.
Neither became a finding only because both reporters posted the broken *pattern*
rather than the number, which is not a property a process can rely on.

So each check needs two halves:

- **positive** — the pattern must match something. Assert each count is `> 0`, not
  only that the three agree.
- **negative** — a deliberately wrong input must produce the *fail* value, which
  catches a pattern matching nothing for a reason unrelated to the tree.

### Where the sanity case has to live

The same shape appears inside test suites, and there it is often already covered — by
structure rather than by design:

| | positive counterpart | consequence |
|---|---|---|
| standalone shell check | none available | must carry its own sanity case inline |
| suite assertion | may borrow one from another test | safe *while that test exists* |

An empty-pass assertion — `queued_rows() == []`, `bytesWritten() == 0` — is safe when
some other test exercises the same helper positively, because a broken helper fails
*there*. That safety is real, and it is invisible at the point of reading.

**Borrowed safety must be written down at the borrower.** The counterpart looks like a
redundant happy-path test; delete it in a tidy-up and the empty-pass assertion keeps
passing, now guarding nothing, with no diff showing that its meaning changed.

```
test_..._queues_nothing_when_payload_is_truncated
    # Safe only because test_..._queues_rows_with_the_battery_column exercises
    # queued_rows() positively. If that test goes, this one passes against a
    # broken helper.
```

The rule in three parts:

- **standalone check** — carries its own sanity case inline
- **suite assertion** — may borrow a positive counterpart from another test
- **borrowed safety** — must be written down at the borrower, or it is one tidy-up
  from gone

This is the same remedy as A27b's DO-NOT-REMOVE comment and as L6 on the backend —
three instances across both halves of one defect: **a test whose value depends on
something outside itself, invisible at the point of reading.**

## Predict catchers, never a pass count

A count of test declarations is not a prediction of a test outcome, and the two look
identical in a message. `grep -c RUN_TEST` returning 73 became "expect 73 passed" in
a hand-off here; the run reported 74 test cases with one failure, and both halves of
the prediction were wrong in a way that read as confident.

Predict which tests must DIE under each mutation and which must LIVE. That is what
the harness rules ask for anyway, and it is the one form a static grep cannot
masquerade as.

(PlatformIO's header counts a program-level entry per FAILING SUITE on top of the
individual results: 73 tests with one failing suite reports 74 test cases, with two
failing suites 75. The suite total and the outcome total are different numbers and
neither is wrong.)

## Assert the shape where the shape is the subject

`test_cycle` holds the worked pair — same file, same round, opposite answers:

- **A21**, the failure side: `bytesWritten() == 0`. Fixture-independent. Nothing is
  written when the connect fails, whatever the pin or the clock do. Keep exactly.
- **A20**, the success side: an exact byte count is a function of pin-and-clock
  arithmetic that has nothing to do with A20's subject, which is that the radio ends
  up off. The wire format already has its guards in `test_transmit` T43 and T44,
  against a ring those tests control directly. Here the count is corroboration.

So the rule is not "counts beat bools". It is: **assert the exact shape where the
shape is the subject, and assert the invariant where it is not** — and where you do
assert a count, derive it from the constant that drives the fixture. A comment
explaining a literal is itself a stand-in for the property; change the fixture and
the prose goes stale with nothing connecting them.

## Stronger is not strictly stronger

An exact byte count detects more than a bool that says a write finished — a bool
cannot see a truncated payload. It is also less stable: a count moves when the
fixture moves, and one in `test_cycle` broke the first time it met a fixture that
stored three frames where the expectation modelled one.

Keep the count. The failure modes are not symmetric:

| | fails when | how it is found |
|---|---|---|
| bool | never, while the code is wrong | an incident, weeks later |
| byte count | at once, while the code is right | the next run, before anything ships |

The mismatch A20 exposed **existed before the count did**. The bool passed whatever
the fixture stored, so the disagreement between the author's model and the fixture sat
there invisibly. The count did not introduce a fragility; it exposed one. A test that
cannot be wrong about its fixture is not a safer test — it is a test that has stopped
asking.

A false negative is silent and survives; a false positive is loud and is paid for
immediately. Buy loud wrongness over quiet rightness. When one of these breaks, fix
the expectation — **derive it from the constant that drives the fixture** — rather
than swapping it back for a bool.

## Checks a test cannot make

Some properties are facts about a declaration rather than about any behaviour a
fake can observe. Writing a test for one of those produces a test that passes
whichever way the declaration goes, which is worse than admitting the gap. Use a
grep, and keep it here where it will be run.

```
grep -c '^RTC_DATA_ATTR' src/app/app.cpp                   # must be exactly 1
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
