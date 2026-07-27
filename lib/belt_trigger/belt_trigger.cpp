#include "belt_trigger.h"

BeltTrigger::BeltTrigger()
    : _lastLevel(TRIGGER_LEVEL_HIGH),
      _openAtMs(0),
      _cooldownSec(0)
{
}

void BeltTrigger::begin(uint32_t nowMs, uint16_t cooldownSec)
{
    // Production starts the task with lastButtonState = HIGH and immediately blocks
    // for one cooldown (main.cpp:309, :314), which is what keeps the magnet that
    // caused the ext0 wake from ending the first acquisition on the first poll.
    _lastLevel = TRIGGER_LEVEL_HIGH;
    _cooldownSec = cooldownSec;
    _openAtMs = nowMs + (uint32_t)cooldownSec * 1000UL;
}

void BeltTrigger::setCooldownSec(uint16_t sec)
{
    _cooldownSec = sec;
}

TriggerEvent BeltTrigger::poll(TriggerLevel raw, uint32_t nowMs)
{
#ifdef MUTANT_COOLDOWN_SAMPLES_THROUGH
    // ==== MUTATION: MUTANT_COOLDOWN_SAMPLES_THROUGH ====
    // Built only by [env:native_mutant]. Never by env:native or env:pico32.
    //
    // BREAKS: the cooldown blocks. This version keeps tracking the pin level while
    //         the window runs and merely suppresses the event, so it reaches the
    //         edge of the window already holding the level the window hid.
    // WHY:    it is the refactor someone will reach for, because the blocking
    //         version looks like a mistake.
    // CAUGHT BY: test_belt_trigger T23, T26 and T26b — each probes the pin at the
    //         exact millisecond the window opens while holding a level that went
    //         LOW inside it. If a change to this class leaves all three green with
    //         this flag on, the change removed the protection, not the bug.
    //
    // Run it:  pio test -e native_mutant
    const bool blocked = ((int32_t)(nowMs - _openAtMs) < 0);

    if (raw == _lastLevel)
        return TRIGGER_EVENT_NONE;

    _lastLevel = raw; // <-- the mutation: level tracked during the window

    if (blocked)
        return TRIGGER_EVENT_NONE;

    if (raw == TRIGGER_LEVEL_LOW)
        return TRIGGER_EVENT_STOP;

    _openAtMs = nowMs + (uint32_t)_cooldownSec * 1000UL;
    return TRIGGER_EVENT_RELEASED;
#else
    // The cooldown BLOCKS. Production sits inside vTaskDelay (main.cpp:336) and
    // does not read the pin at all, so the remembered level survives the window
    // untouched and the first poll after it compares against a level from before
    // it. Wrap-safe subtraction: the original's equivalent check at
    // ICM42688P.cpp:436 misfires past 71.6 minutes for want of one.
    if ((int32_t)(nowMs - _openAtMs) < 0)
        return TRIGGER_EVENT_NONE;

    if (raw == _lastLevel)
        return TRIGGER_EVENT_NONE;

    _lastLevel = raw;

    // Magnet arrives: end the acquisition. One belt revolution, one acquisition.
    if (raw == TRIGGER_LEVEL_LOW)
        return TRIGGER_EVENT_STOP;

    // Magnet leaves: that is what arms the cooldown (main.cpp:334-337).
    _openAtMs = nowMs + (uint32_t)_cooldownSec * 1000UL;
    return TRIGGER_EVENT_RELEASED;
#endif
}
