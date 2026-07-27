#ifndef BELT_TRIGGER_H
#define BELT_TRIGGER_H

#include <stdint.h>

// Decides whether a magnet edge means "end this acquisition". Pure — no Arduino,
// no clock interface: poll() is handed the timestamp, so there is nothing left for
// a seam to abstract. Reading GPIO33 stays outside, in the Arduino wiring.
//
// Replaces the FreeRTOS buttonTask at main.cpp:305-345. Faithful to it, including
// the parts that look like mistakes:
//
//   - There is NO debounce, and that is a decision rather than an oversight on our
//     part. main.cpp:307 comments one, :311 and :312 declare lastDebounceTime and
//     timerStarted, and nothing ever reads either; the state test at :322 is a bare
//     edge detect and the 1 ms delay at :340 is the poll period, not a filter. The
//     shipping firmware has behaved this way for its whole service life, so a
//     bouncing reed switch fires the stop on the first LOW and the bounce back to
//     HIGH then takes the release branch and blocks for a full cooldown — the
//     bounce costs a blind window, not a spurious stop. That consequence is
//     accepted. Do not add a filter here, and do not add a config field for one:
//     it would change what the belt sees, with no defect to point at.
//   - The cooldown BLOCKS rather than filters. :336 is a vTaskDelay inside the
//     task, so the pin is not read at all while it runs: a magnet pass inside the
//     window is missed, not suppressed, and the level after the window is compared
//     against the one remembered from before it.
//   - The initial cooldown at :314 is load-bearing, not a stray sleep. The magnet
//     that fired the ext0 wake is still over the reed switch when the task starts
//     with a remembered level of HIGH; without the delay the first poll would end
//     the acquisition instantly.

enum TriggerLevel : uint8_t
{
    TRIGGER_LEVEL_LOW = 0, // magnet present — the reed switch pulls the pin down
    TRIGGER_LEVEL_HIGH
};

enum TriggerEvent : uint8_t
{
    TRIGGER_EVENT_NONE = 0,
    TRIGGER_EVENT_STOP,    // end the current acquisition
    TRIGGER_EVENT_RELEASED // magnet left; the cooldown starts here
};

class BeltTrigger
{
public:
    BeltTrigger();

    // Starts the initial cooldown running from nowMs.
    void begin(uint32_t nowMs, uint16_t cooldownSec);

    // Takes effect on the next cooldown, the way production re-reads
    // response.interTriggerTime at each vTaskDelay.
    void setCooldownSec(uint16_t sec);

    // Feed it the raw pin level and the current millis(). While the cooldown runs,
    // the level is not looked at — see the note above about blocking vs filtering.
    TriggerEvent poll(TriggerLevel raw, uint32_t nowMs);

private:
    TriggerLevel _lastLevel;
    uint32_t _openAtMs; // window is half-open: blocked while now < _openAtMs
    uint16_t _cooldownSec;
};

#endif // BELT_TRIGGER_H
