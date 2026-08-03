#ifndef SIF_LOG_H
#define SIF_LOG_H

// Serial diagnostics for the pure modules under lib/.
//
// THIS IS THE ONE PLACE lib/ REACHES ARDUINO, and it is deliberate. Production
// prints through the whole cycle and the refactor dropped nearly all of it, which
// left a device on the bench failing silently with no way to tell WHERE it stopped.
// Restoring those prints at their equivalent points means printing from code that
// now lives in lib/ — the trigger edges, the acquisition timing, the transmission
// results — and moving that logic back into src/ to accommodate a print would be
// refactoring to make room for instrumentation.
//
// Everything below compiles to nothing off-target, so env:native still builds these
// modules without Arduino and the layout rule holds for every other header. The
// alternative is an ILogger threaded through four constructors, which is a
// structural change, and this was added to diagnose a live failure rather than to
// improve a design. Swap it for the seam later if the prints outlive the bug.

#ifdef ARDUINO
#include <Arduino.h>
#define SIF_LOG(msg) Serial.println(msg)
#define SIF_LOG_RAW(msg) Serial.print(msg)
#define SIF_LOGF(...) Serial.printf(__VA_ARGS__)
#define SIF_TIMER(name) uint32_t name = millis()
#define SIF_ELAPSED(name) (uint32_t)(millis() - (name))
#else
#define SIF_LOG(msg) ((void)0)
#define SIF_LOG_RAW(msg) ((void)0)
#define SIF_LOGF(...) ((void)0)
#define SIF_TIMER(name) ((void)0)
#define SIF_ELAPSED(name) 0u
#endif

#endif // SIF_LOG_H
