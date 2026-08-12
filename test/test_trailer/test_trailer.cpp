// The uplink trailer: what the sensor says about itself when a capture ends.
//
// Until now the message ended with two bytes of battery. That left two questions
// no one on the server could answer: which firmware is this sensor running, and
// what rate did it ACTUALLY achieve — not the rate it was told to run. The server
// knew what it had asked for and nothing about what happened.
//
// Seven bytes now: battery, three of version, and the measured rate. The layout
// is fixed and the server accepts exactly 7 or exactly 2 (a fleet still running
// the old firmware), never anything in between.

#include <unity.h>
#include <string.h>

#include "packet.h"

void setUp(void) {}
void tearDown(void) {}

static void test_the_trailer_is_seven_bytes(void)
{
    TEST_ASSERT_EQUAL_UINT32(7, (uint32_t)TRAILER_SIZE_BYTES);
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)LEGACY_TRAILER_SIZE_BYTES);
}

static void test_the_battery_keeps_the_first_two_bytes_it_always_had(void)
{
    // A server reading only the first two bytes of the new trailer still finds
    // the battery exactly where the old one put it.
    uint8_t out[TRAILER_SIZE_BYTES];

    pack_trailer(out, 4100, 1, 2, 3, 199);

    TEST_ASSERT_EQUAL_UINT16(4100, (uint16_t)(out[0] | (out[1] << 8)));
}

static void test_version_and_measured_rate_follow_the_battery(void)
{
    uint8_t out[TRAILER_SIZE_BYTES];

    pack_trailer(out, 4100, 1, 2, 3, 199);

    TEST_ASSERT_EQUAL_UINT8(1, out[2]);
    TEST_ASSERT_EQUAL_UINT8(2, out[3]);
    TEST_ASSERT_EQUAL_UINT8(3, out[4]);
    TEST_ASSERT_EQUAL_UINT16(199, (uint16_t)(out[5] | (out[6] << 8)));
}

static void test_the_firmware_version_is_the_one_the_build_carries(void)
{
    // One source. The version used to be a string literal in the HTTP handler and
    // did not exist on the wire at all, so two places could disagree about which
    // firmware a device was running.
    uint8_t out[TRAILER_SIZE_BYTES];

    pack_trailer_for_this_build(out, 4100, 199);

    TEST_ASSERT_EQUAL_UINT8(SIF_FW_VERSION_MAJOR, out[2]);
    TEST_ASSERT_EQUAL_UINT8(SIF_FW_VERSION_MINOR, out[3]);
    TEST_ASSERT_EQUAL_UINT8(SIF_FW_VERSION_PATCH, out[4]);
}

// ---------------------------------------------------------------------------
// The measured rate
// ---------------------------------------------------------------------------

static void test_the_rate_is_what_was_measured_not_what_was_asked_for(void)
{
    // 1000 frames over 5 seconds is 200 Hz. A sensor told to run 200 and
    // achieving 199 is exactly the discrepancy this field exists to expose.
    TEST_ASSERT_EQUAL_UINT16(200, effective_hz(1000, 5000));
    TEST_ASSERT_EQUAL_UINT16(199, effective_hz(995, 5000));
}

static void test_a_capture_with_no_duration_reports_nothing(void)
{
    // Not infinity, and not a division by zero: an acquisition that stopped in
    // the same millisecond it started measured no rate at all.
    TEST_ASSERT_EQUAL_UINT16(0, effective_hz(1000, 0));
}

static void test_an_empty_capture_reports_zero(void)
{
    TEST_ASSERT_EQUAL_UINT16(0, effective_hz(0, 5000));
}

static void test_the_rate_is_rounded_rather_than_truncated(void)
{
    // 62 frames in 1000 ms is 62 Hz; 63 in 1005 ms is 62.7, which is 63 to
    // anyone reading it. Truncation would print a rate consistently low.
    TEST_ASSERT_EQUAL_UINT16(62, effective_hz(62, 1000));
    TEST_ASSERT_EQUAL_UINT16(63, effective_hz(63, 1005));
}

static void test_an_absurd_rate_is_clamped_instead_of_wrapping(void)
{
    // The field is a uint16. A wrap would report 200 Hz as something small and
    // plausible, which is worse than a number that is obviously pinned.
    TEST_ASSERT_EQUAL_UINT16(65535, effective_hz(4000000, 1000));
}

static int run_all(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_trailer_is_seven_bytes);
    RUN_TEST(test_the_battery_keeps_the_first_two_bytes_it_always_had);
    RUN_TEST(test_version_and_measured_rate_follow_the_battery);
    RUN_TEST(test_the_firmware_version_is_the_one_the_build_carries);
    RUN_TEST(test_the_rate_is_what_was_measured_not_what_was_asked_for);
    RUN_TEST(test_a_capture_with_no_duration_reports_nothing);
    RUN_TEST(test_an_empty_capture_reports_zero);
    RUN_TEST(test_the_rate_is_rounded_rather_than_truncated);
    RUN_TEST(test_an_absurd_rate_is_clamped_instead_of_wrapping);
    return UNITY_END();
}

#ifdef ARDUINO
#include <Arduino.h>
void setup() { delay(2000); run_all(); }
void loop() {}
#else
int main(void) { return run_all(); }
#endif
