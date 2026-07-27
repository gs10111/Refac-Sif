#include "belt_cycle.h"

BeltStep belt_next(BeltStage current, const BeltInputs &in)
{
    // Outranks everything: an armed device serves the upload page instead of
    // starting a cycle. The stage it leaves behind does not matter — entering OTA
    // ends in a restart either way, and a restart reloads the stage from flash.
    if (in.otaFlagSet)
        return {BELT_STAGE_ARM_TRIGGER, BELT_ACTION_ENTER_OTA};

    switch (current)
    {
    case BELT_STAGE_ARM_TRIGGER:
        // Arm ext0 and sleep. The magnet wake resumes at CYCLE.
        return {BELT_STAGE_CYCLE, BELT_ACTION_SLEEP_UNTIL_TRIGGER};

    case BELT_STAGE_CYCLE:
        // Ahead of the sleep decision, so an update arriving on the last
        // acquisition of a cycle is not swallowed by the 240-minute sleep.
        if (in.updateRequestedByServer)
            return {BELT_STAGE_ARM_TRIGGER, BELT_ACTION_RESTART};

        // Either ends the cycle: the quota is filled, or the belt stopped.
        if (in.acquisitionsDone >= in.maxAcquisitions || in.idleTimedOut)
            return {BELT_STAGE_ARM_TRIGGER, BELT_ACTION_SLEEP_TIMER};

        return {BELT_STAGE_CYCLE, BELT_ACTION_RUN_CYCLE_ITERATION};

    case BELT_STAGE_COUNT:
    default:
        // Not reachable from a stage this code wrote, but RTC memory is one
        // corrupted byte away from getting here. Fall back to waiting for the
        // belt — never to a state with no action, which is what the refactor's
        // empty STAGE_ACQUIRE_TRANSMIT case did.
        return {BELT_STAGE_ARM_TRIGGER, BELT_ACTION_SLEEP_UNTIL_TRIGGER};
    }
}
