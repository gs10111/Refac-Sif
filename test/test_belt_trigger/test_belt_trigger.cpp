// Round 3 — T22..T26b — the magnet trigger.
//
// This is the FreeRTOS buttonTask the refactor deleted (main.cpp:305-345), and with
// it the reason trigger_cooldown_sec has been dead config ever since: nothing calls
// AcquisitionService::stop(), so an acquisition is never bounded by a belt
// revolution.
//
// The judgement is pulled out of the pin read on purpose. Reading GPIO33 is
// hardware; deciding that an edge means "stop" is not, and it is the part with the
// bugs in it. This class takes a raw level and a timestamp and returns an event.
//
// Production behaviour, faithfully:
//
//   main.cpp:314   vTaskDelay(interTriggerTime * 1000)   before the polling loop
//   main.cpp:319   digitalRead every 1 ms
//   main.cpp:322   bare edge detect against lastButtonState
//   main.cpp:330   LOW  -> stopFlag = true
//   main.cpp:336   HIGH -> vTaskDelay(interTriggerTime * 1000), BLOCKING
//
// Two consequences that shape every test below:
//
//  1. The cooldown BLOCKS. Production sits inside vTaskDelay and does not read the
//     pin at all, so a whole magnet pass inside the window is missed — not
//     filtered, missed. When the delay returns it reads the pin fresh against a
//     lastButtonState of HIGH, so a level that went LOW during the window fires a
//     stop on the very first poll after it.
//
//  2. The initial delay at :314 is load-bearing. The task starts with
//     lastButtonState = HIGH while the magnet that caused the ext0 wake is still
//     over the reed switch. Without it the first poll would see HIGH -> LOW and end
//     the acquisition instantly.

#include <unity.h>
#include <stdint.h>

#include "packet.h"
#include "belt_trigger.h"

void setUp(void) {}
void tearDown(void) {}

// T22 — the edge that ends an acquisition: the next magnet coming round.
static void test_falling_edge_requests_stop(void)
{
    BeltTrigger trigger;
    trigger.begin(0, DEFAULT_TRIGGER_COOLDOWN_SEC);

    // Window opens at exactly begin + cooldown.
    TEST_ASSERT_EQUAL_UINT8(TRIGGER_EVENT_NONE, trigger.poll(TRIGGER_LEVEL_HIGH, 5000));
    TEST_ASSERT_EQUAL_UINT8(TRIGGER_EVENT_STOP, trigger.poll(TRIGGER_LEVEL_LOW, 5100));

    // A held level is an edge once, not on every poll.
    TEST_ASSERT_EQUAL_UINT8(TRIGGER_EVENT_NONE, trigger.poll(TRIGGER_LEVEL_LOW, 5200));
}

// T23 — replaces the debounce test from the seam plan: production has NO debounce.
// What it does have is the initial blocking delay, and that one is load-bearing —
// it stops the magnet that woke the device from instantly ending the acquisition.
static void test_stop_is_not_requested_during_the_initial_cooldown(void)
{
    BeltTrigger trigger;
    trigger.begin(0, DEFAULT_TRIGGER_COOLDOWN_SEC);

    // The magnet that fired ext0 is still sitting over the reed switch.
    TEST_ASSERT_EQUAL_UINT8(TRIGGER_EVENT_NONE, trigger.poll(TRIGGER_LEVEL_LOW, 0));
    TEST_ASSERT_EQUAL_UINT8(TRIGGER_EVENT_NONE, trigger.poll(TRIGGER_LEVEL_LOW, 4999));

    // The moment the window opens the pin is read fresh against HIGH, so a belt
    // that stopped with the magnet parked does end the acquisition immediately.
    TEST_ASSERT_EQUAL_UINT8(TRIGGER_EVENT_STOP, trigger.poll(TRIGGER_LEVEL_LOW, 5000));
}

// T24 — the cooldown itself: one revolution per acquisition, not one per bounce.
static void test_second_edge_within_cooldown_seconds_is_ignored(void)
{
    BeltTrigger trigger;
    trigger.begin(0, DEFAULT_TRIGGER_COOLDOWN_SEC);

    trigger.poll(TRIGGER_LEVEL_HIGH, 5000);
    TEST_ASSERT_EQUAL_UINT8(TRIGGER_EVENT_STOP, trigger.poll(TRIGGER_LEVEL_LOW, 5100));

    // The magnet leaves: that is what arms the cooldown, running to 10200.
    TEST_ASSERT_EQUAL_UINT8(TRIGGER_EVENT_RELEASED, trigger.poll(TRIGGER_LEVEL_HIGH, 5200));

    TEST_ASSERT_EQUAL_UINT8(TRIGGER_EVENT_NONE, trigger.poll(TRIGGER_LEVEL_LOW, 6000));
    TEST_ASSERT_EQUAL_UINT8(TRIGGER_EVENT_NONE, trigger.poll(TRIGGER_LEVEL_LOW, 10199));
}

// T25
static void test_trigger_rearms_after_cooldown_elapses(void)
{
    BeltTrigger trigger;
    trigger.begin(0, DEFAULT_TRIGGER_COOLDOWN_SEC);

    trigger.poll(TRIGGER_LEVEL_HIGH, 5000);
    trigger.poll(TRIGGER_LEVEL_LOW, 5100);
    trigger.poll(TRIGGER_LEVEL_HIGH, 5200);

    TEST_ASSERT_EQUAL_UINT8(TRIGGER_EVENT_STOP, trigger.poll(TRIGGER_LEVEL_LOW, 10200));
}

// T26 — the field has been dead since the refactor. It must come from the config,
// and a new value must take effect on the next cooldown, the way production
// re-reads response.interTriggerTime at each vTaskDelay.
static void test_cooldown_uses_configured_seconds_not_a_constant(void)
{
    BeltTrigger trigger;
    trigger.begin(0, 12);

    TEST_ASSERT_EQUAL_UINT8(TRIGGER_EVENT_NONE, trigger.poll(TRIGGER_LEVEL_LOW, 11999));
    TEST_ASSERT_EQUAL_UINT8(TRIGGER_EVENT_STOP, trigger.poll(TRIGGER_LEVEL_LOW, 12000));

    trigger.setCooldownSec(1);

    TEST_ASSERT_EQUAL_UINT8(TRIGGER_EVENT_RELEASED, trigger.poll(TRIGGER_LEVEL_HIGH, 12100));
    TEST_ASSERT_EQUAL_UINT8(TRIGGER_EVENT_NONE, trigger.poll(TRIGGER_LEVEL_LOW, 13099));
    TEST_ASSERT_EQUAL_UINT8(TRIGGER_EVENT_STOP, trigger.poll(TRIGGER_LEVEL_LOW, 13100));
}

// T26b — the cooldown blocks, it does not filter. A full magnet pass inside the
// window is missed entirely, and the level at the end of the window is compared
// against the level from before it. Anyone who "improves" this by tracking levels
// during the cooldown changes what the belt sees.
static void test_levels_are_not_observed_during_cooldown(void)
{
    BeltTrigger trigger;
    trigger.begin(0, DEFAULT_TRIGGER_COOLDOWN_SEC);

    trigger.poll(TRIGGER_LEVEL_HIGH, 5000);
    trigger.poll(TRIGGER_LEVEL_LOW, 5100);
    trigger.poll(TRIGGER_LEVEL_HIGH, 5200); // cooldown runs to 10200

    // A magnet arrives inside the window and stays. Production is sitting inside
    // vTaskDelay, so it is missed rather than filtered.
    TEST_ASSERT_EQUAL_UINT8(TRIGGER_EVENT_NONE, trigger.poll(TRIGGER_LEVEL_LOW, 6000));
    TEST_ASSERT_EQUAL_UINT8(TRIGGER_EVENT_NONE, trigger.poll(TRIGGER_LEVEL_LOW, 10199));

    // The instant the window opens the pin is read fresh against the level
    // remembered from BEFORE it — still HIGH, from 5200 — so the magnet that has
    // been sitting there since 6000 fires now.
    //
    // The asymmetry matters. An implementation that samples through the window and
    // merely suppresses the event arrives here already holding LOW, sees no edge,
    // and stays silent. That is the one assertion separating the two.
    TEST_ASSERT_EQUAL_UINT8(TRIGGER_EVENT_STOP, trigger.poll(TRIGGER_LEVEL_LOW, 10200));
}

static int run_all(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_falling_edge_requests_stop);
    RUN_TEST(test_stop_is_not_requested_during_the_initial_cooldown);
    RUN_TEST(test_second_edge_within_cooldown_seconds_is_ignored);
    RUN_TEST(test_trigger_rearms_after_cooldown_elapses);
    RUN_TEST(test_cooldown_uses_configured_seconds_not_a_constant);
    RUN_TEST(test_levels_are_not_observed_during_cooldown);
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
