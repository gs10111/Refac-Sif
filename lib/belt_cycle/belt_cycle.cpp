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
#ifdef MUTANT_MAX_ACQ_OFF_BY_ONE
        // ==== MUTATION: MUTANT_MAX_ACQ_OFF_BY_ONE ====
        // Built only by [env:mutant_max_acq_off_by_one].
        // Never by env:native or env:pico32.
        // Run it:  pio test -e mutant_max_acq_off_by_one
        //
        // BREAKS: `>` for `>=`, so the cycle runs one acquisition more than
        //         max_acquisitions — six uploads where the operator configured five.
        // WHY:    it is the classic off-by-one on the one number bigboss specified,
        //         and nothing about the data looks wrong. It costs an extra
        //         acquisition of battery per cycle, forever.
        // CAUGHT BY: test_belt_cycle T15, which probes done == max exactly, AND
        //         T31d — with `>`, max_acq == 0 no longer ends the cycle, so a
        //         device configured to collect nothing would collect. T31d was
        //         written as a characterisation test in round 4, passing on
        //         arrival with nothing driving it; it turned out to be a live
        //         detector for a mutation introduced five rounds later. A run that
        //         kills only T15 is a REGRESSION, not a pass.
        // SURVIVED BY: T14, which only walks done from 1 to max-1 and would agree
        //         with either operator.
        if (in.acquisitionsDone > in.maxAcquisitions || in.idleTimedOut)
#else
        if (in.acquisitionsDone >= in.maxAcquisitions || in.idleTimedOut)
#endif
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
