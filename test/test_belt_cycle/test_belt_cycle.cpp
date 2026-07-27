// Round 2 — T12..T21 — the acquisition cycle state machine.
//
// This is where D1 lives: the device stays AWAKE between acquisitions.
//
//   magnet wake -> CPU 10 MHz -> { collect until the trigger stops it ->
//   CPU 240 MHz -> transmit -> read config -> CPU 10 MHz } repeated in the SAME
//   wake, until max_acq acquisitions are done OR the idle timeout elapses with no
//   trigger -> only then deep sleep for sleep_min by timer -> on that wake, arm
//   ext0 and sleep waiting for the magnet.
//
// The refactor deep-slept after every single transmit, which is why the
// acquisition counter never got past 1 and max_acquisitions was unreachable.
//
// Two stages are enough, and they mean "what to do on the next wake":
//   ARM_TRIGGER — arm ext0 and deep sleep until the magnet fires
//   CYCLE       — awake, running acquisitions back to back
// There is no SLEEP_TIMER stage: the timer sleep is a transition, not a state.
// The stage is set to ARM_TRIGGER before sleeping, so the timer wake arms ext0 —
// the same ladder the production firmware walks.
//
// OTA is NOT a stage. Its flag has to survive the ESP.restart() that arms it, and
// RTC memory does not — .rtc.data is reloaded from flash on every reset except a
// deep-sleep wake. So it lives in NVS and enters here as an input.

#include <unity.h>
#include <stdint.h>

#include "packet.h"
#include "belt_cycle.h"

void setUp(void) {}
void tearDown(void) {}

// Nothing pending, nothing collected — the state a cycle starts from.
static BeltInputs fresh_inputs(void)
{
    BeltInputs in;
    in.otaFlagSet = false;
    in.acquisitionsDone = 0;
    in.maxAcquisitions = DEFAULT_MAX_ACQUISITIONS;
    in.idleTimedOut = false;
    in.updateRequestedByServer = false;
    return in;
}

// T12 — cold boot. The initial stage must be the safe one: arm the trigger and
// sleep, never resume mid-cycle.
static void test_cold_boot_arms_trigger_and_sleeps(void)
{
    TEST_ASSERT_EQUAL_UINT8(BELT_STAGE_ARM_TRIGGER, BELT_INITIAL_STAGE);

    BeltStep step = belt_next(BELT_INITIAL_STAGE, fresh_inputs());

    TEST_ASSERT_EQUAL_UINT8(BELT_ACTION_SLEEP_UNTIL_TRIGGER, step.action);
    TEST_ASSERT_EQUAL_UINT8(BELT_STAGE_CYCLE, step.next);
}

// T13 — the magnet wake resumes at CYCLE with nothing collected. The first thing
// that happens must be an acquisition, not another sleep.
static void test_trigger_wake_enters_cycle(void)
{
    BeltStep step = belt_next(BELT_STAGE_CYCLE, fresh_inputs());

    TEST_ASSERT_EQUAL_UINT8(BELT_ACTION_RUN_CYCLE_ITERATION, step.action);
    TEST_ASSERT_EQUAL_UINT8(BELT_STAGE_CYCLE, step.next);
}

// T14 — the headline D1 test. After a transmit, with acquisitions still owed, the
// device runs the next one in the same wake. Deep sleeping here is the regression.
static void test_cycle_stays_in_cycle_after_transmit_when_acquisitions_remain(void)
{
    for (uint16_t done = 1; done < DEFAULT_MAX_ACQUISITIONS; done++)
    {
        BeltInputs in = fresh_inputs();
        in.acquisitionsDone = done;

        BeltStep step = belt_next(BELT_STAGE_CYCLE, in);

        TEST_ASSERT_EQUAL_UINT8(BELT_ACTION_RUN_CYCLE_ITERATION, step.action);
        TEST_ASSERT_EQUAL_UINT8(BELT_STAGE_CYCLE, step.next);
    }
}

// T15 — max_acq reached ends the cycle with the timer sleep, and leaves the stage
// at ARM_TRIGGER so the next wake arms ext0.
static void test_cycle_sleeps_by_timer_after_max_acquisitions(void)
{
    BeltInputs in = fresh_inputs();
    in.acquisitionsDone = DEFAULT_MAX_ACQUISITIONS;

    BeltStep step = belt_next(BELT_STAGE_CYCLE, in);
    TEST_ASSERT_EQUAL_UINT8(BELT_ACTION_SLEEP_TIMER, step.action);
    TEST_ASSERT_EQUAL_UINT8(BELT_STAGE_ARM_TRIGGER, step.next);

    // A counter that overshot must not wrap the device back into collecting.
    in.acquisitionsDone = DEFAULT_MAX_ACQUISITIONS + 1;
    step = belt_next(BELT_STAGE_CYCLE, in);
    TEST_ASSERT_EQUAL_UINT8(BELT_ACTION_SLEEP_TIMER, step.action);
}

// T16 — the belt stopped: the last acquisition ran the full idle timeout without a
// trigger. That ends the cycle too, whatever the counter says.
static void test_cycle_sleeps_by_timer_after_idle_timeout_without_trigger(void)
{
    BeltInputs in = fresh_inputs();
    in.acquisitionsDone = 1;
    in.idleTimedOut = true;

    BeltStep step = belt_next(BELT_STAGE_CYCLE, in);

    TEST_ASSERT_EQUAL_UINT8(BELT_ACTION_SLEEP_TIMER, step.action);
    TEST_ASSERT_EQUAL_UINT8(BELT_STAGE_ARM_TRIGGER, step.next);
}

// T17 — waking from the sleep_min timer sleep. The stage was left at ARM_TRIGGER,
// so this wake arms ext0 and sleeps again. A stale counter must not change it.
static void test_timer_wake_arms_trigger(void)
{
    BeltInputs in = fresh_inputs();
    in.acquisitionsDone = DEFAULT_MAX_ACQUISITIONS;

    BeltStep step = belt_next(BELT_STAGE_ARM_TRIGGER, in);

    TEST_ASSERT_EQUAL_UINT8(BELT_ACTION_SLEEP_UNTIL_TRIGGER, step.action);
    TEST_ASSERT_EQUAL_UINT8(BELT_STAGE_CYCLE, step.next);
}

// T18 — a panic or brownout mid-cycle reloads .rtc.data from flash, so the stage
// goes back to its initial value and the RAM-resident counter is gone. The device
// must fall back to waiting for the belt, not resume as if collecting.
static void test_unexpected_reset_lands_in_arm_trigger(void)
{
    TEST_ASSERT_EQUAL_UINT8(BELT_STAGE_ARM_TRIGGER, BELT_INITIAL_STAGE);

    BeltInputs in = fresh_inputs();
    in.acquisitionsDone = 3; // stale, and about to be discarded with the RAM

    BeltStep step = belt_next(BELT_INITIAL_STAGE, in);

    TEST_ASSERT_EQUAL_UINT8(BELT_ACTION_SLEEP_UNTIL_TRIGGER, step.action);
}

// T19 — the refactor had `case STAGE_ACQUIRE_TRANSMIT: break;`, a stage that
// produced no action and no sleep. No stage may fall through to INVALID, and no
// transition may name a stage that does not exist.
static void test_every_stage_is_handled(void)
{
    BeltInputs in = fresh_inputs();

    for (uint8_t s = 0; s < BELT_STAGE_COUNT; s++)
    {
        BeltStep step = belt_next((BeltStage)s, in);

        TEST_ASSERT_NOT_EQUAL(BELT_ACTION_INVALID, step.action);
        TEST_ASSERT_TRUE(step.next < BELT_STAGE_COUNT);
    }
}

// T20 — D4. The OTA flag is read from NVS at boot and outranks everything: an
// armed device serves the upload page instead of starting a cycle.
static void test_ota_flag_at_boot_routes_to_ota_before_any_acquisition(void)
{
    BeltInputs in = fresh_inputs();
    in.otaFlagSet = true;

    TEST_ASSERT_EQUAL_UINT8(BELT_ACTION_ENTER_OTA, belt_next(BELT_STAGE_CYCLE, in).action);
    TEST_ASSERT_EQUAL_UINT8(BELT_ACTION_ENTER_OTA, belt_next(BELT_STAGE_ARM_TRIGGER, in).action);
}

// T21 — D3. update=1 in the response persists the request and restarts into OTA.
// It has to outrank the sleep decision: an update arriving on the last acquisition
// of a cycle would otherwise be swallowed by the 240-minute sleep.
static void test_server_update_flag_routes_to_restart(void)
{
    BeltInputs in = fresh_inputs();
    in.updateRequestedByServer = true;
    in.acquisitionsDone = DEFAULT_MAX_ACQUISITIONS;

    BeltStep step = belt_next(BELT_STAGE_CYCLE, in);

    TEST_ASSERT_EQUAL_UINT8(BELT_ACTION_RESTART, step.action);
    TEST_ASSERT_EQUAL_UINT8(BELT_STAGE_ARM_TRIGGER, step.next);
}

// T31d — max_acq of 0 ends the cycle immediately, having collected nothing.
//
// CHARACTERISATION TEST: this already passes. It is here because the behaviour was
// emergent from `0 >= 0` rather than specified, and it happens to match production
// by coincidence of a different mechanism — production pre-increments to 1 on entry
// and breaks on `loopCounter > nSamples`, so 1 > 0 fires on the first iteration and
// the device sleeps having collected essentially nothing. Same observable outcome,
// so DEC-0 settles it: no clamp, no reinterpreting 0 as unlimited in the firmware.
// The web form refuses 0, which is where an operator should be stopped from
// disabling a device for four hours at a time.
static void test_max_acquisitions_of_zero_ends_the_cycle_immediately(void)
{
    BeltInputs in = fresh_inputs();
    in.maxAcquisitions = 0;
    in.acquisitionsDone = 0;

    BeltStep step = belt_next(BELT_STAGE_CYCLE, in);

    TEST_ASSERT_EQUAL_UINT8(BELT_ACTION_SLEEP_TIMER, step.action);
    TEST_ASSERT_EQUAL_UINT8(BELT_STAGE_ARM_TRIGGER, step.next);
}

static int run_all(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_max_acquisitions_of_zero_ends_the_cycle_immediately);
    RUN_TEST(test_cold_boot_arms_trigger_and_sleeps);
    RUN_TEST(test_trigger_wake_enters_cycle);
    RUN_TEST(test_cycle_stays_in_cycle_after_transmit_when_acquisitions_remain);
    RUN_TEST(test_cycle_sleeps_by_timer_after_max_acquisitions);
    RUN_TEST(test_cycle_sleeps_by_timer_after_idle_timeout_without_trigger);
    RUN_TEST(test_timer_wake_arms_trigger);
    RUN_TEST(test_unexpected_reset_lands_in_arm_trigger);
    RUN_TEST(test_every_stage_is_handled);
    RUN_TEST(test_ota_flag_at_boot_routes_to_ota_before_any_acquisition);
    RUN_TEST(test_server_update_flag_routes_to_restart);
    return UNITY_END();
}

#ifdef ARDUINO
#include <Arduino.h>
void setup()
{
    delay(2000);
    run_all();
}
void loop() {}
#else
int main(void)
{
    return run_all();
}
#endif
