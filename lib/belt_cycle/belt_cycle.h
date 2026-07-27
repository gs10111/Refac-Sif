#ifndef BELT_CYCLE_H
#define BELT_CYCLE_H

#include <stdint.h>

// Acquisition cycle decision table. Pure — no Arduino, no ESP headers, no clock.
//
// Two stages, each meaning "what to do on the next wake":
//   ARM_TRIGGER  arm ext0 on the magnet pin and deep sleep
//   CYCLE        awake, running acquisitions back to back
//
// The timer sleep is a transition, not a stage: the stage is set to ARM_TRIGGER
// before sleeping, so the timer wake arms ext0. Production does the same at
// main.cpp:172.
//
// The stage is held in RTC memory, which survives a deep-sleep wake but is
// reloaded from flash on every other reset — so BELT_INITIAL_STAGE is also the
// post-crash state and has to be the safe one.
enum BeltStage : uint8_t
{
    BELT_STAGE_ARM_TRIGGER = 0,
    BELT_STAGE_CYCLE,
    BELT_STAGE_COUNT
};

enum BeltAction : uint8_t
{
    BELT_ACTION_INVALID = 0, // never returned; a stage that falls through is a bug
    BELT_ACTION_SLEEP_UNTIL_TRIGGER,
    BELT_ACTION_RUN_CYCLE_ITERATION,
    BELT_ACTION_SLEEP_TIMER,
    BELT_ACTION_ENTER_OTA,
    BELT_ACTION_RESTART
};

struct BeltInputs
{
    // Read from NVS, not RTC memory: the flag exists to survive the ESP.restart()
    // that arms it, and RTC memory does not.
    bool otaFlagSet;

    // Cycle iterations ATTEMPTED in this wake — NOT successful transmissions.
    //
    // Read that again before you change anything here. "5 acquisitions" reads like
    // "5 uploads", and it is not. Production increments at ICM42688P.cpp:363, on
    // entry to the collect, before anything downstream can fail; when the WiFi
    // connect times out, main.cpp:212-219 returns out of loop() without
    // transmitting and the next iteration increments all the same.
    //
    // Counting successes instead would hang the device in the field: with the
    // server unreachable, max_acquisitions is never reached, the cycle never ends,
    // and it collects and fails and collects again at full draw until the battery
    // is flat. Every happy-path test stays green while it happens, and the symptom
    // on the bench looks like a hardware fault.
    //
    // So: increment unconditionally, once per iteration, whatever the transmit did.
    uint16_t acquisitionsDone;
    uint16_t maxAcquisitions;

    // The last acquisition ran the full idle timeout without a trigger — the belt
    // stopped. Reported by the acquisition, not measured here: production checks it
    // inside the sampling loop against that acquisition's own start time
    // (ICM42688P.cpp:436), so it is a property of one acquisition, not of the wake.
    bool idleTimedOut;

    // The last response carried update=1.
    bool updateRequestedByServer;
};

struct BeltStep
{
    BeltStage next;
    BeltAction action;
};

// Also the state after any reset that is not a deep-sleep wake, so it must be the
// safe one: wait for the belt rather than resume mid-cycle.
constexpr BeltStage BELT_INITIAL_STAGE = BELT_STAGE_ARM_TRIGGER;

// Precedence: otaFlagSet > updateRequestedByServer > (max reached | idle timeout)
// > run another acquisition.
BeltStep belt_next(BeltStage current, const BeltInputs &in);

#endif // BELT_CYCLE_H
