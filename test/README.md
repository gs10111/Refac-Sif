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

## Ground rules

Tests come first — a failing test before the production code that satisfies it.

Where a test pins behaviour that matches the original production firmware rather
than behaviour we think is better, the comment says so. The original is in
production and works, defects included; matching it is the tie-breaker.
