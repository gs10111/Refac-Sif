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
pio test -e native_mutant
```

A green suite proves the tests *can* pass. It does not prove they can *detect*
anything — a test whose subject is an absence ("this does not happen") is
satisfied by an implementation that does nothing at all. The only way to tell the
two apart is to run the tests against an implementation that is known to be wrong.

`env:native_mutant` builds the same tests against deliberately broken code, guarded
by `-DMUTANT_*` flags. **Failures are the pass condition.** If a mutation runs green,
the tests that were supposed to catch it are documenting an intention rather than
enforcing a behaviour.

| Flag | Breaks | Caught by |
|---|---|---|
| `MUTANT_COOLDOWN_SAMPLES_THROUGH` | `BeltTrigger::poll` tracks the pin level during the cooldown and suppresses the event, instead of not reading the pin at all | `test_belt_trigger` T23, T26, T26b |

Each flag is documented at the `#ifdef` that implements it. Rules:

- `env:native` and `env:pico32` never compile a mutant branch.
- Choose and name the mutation **before** running it. One picked after seeing which
  tests are weak proves nothing.
- Don't edit a test to catch a mutation you only predicted. Get the observed output
  first — otherwise you have tuned the test to your imagination.

## Ground rules

Tests come first — a failing test before the production code that satisfies it.

Where a test pins behaviour that matches the original production firmware rather
than behaviour we think is better, the comment says so. The original is in
production and works, defects included; matching it is the tie-breaker.
