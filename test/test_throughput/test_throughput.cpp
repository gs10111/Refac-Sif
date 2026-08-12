// The throughput number has to describe the transmission and nothing else.
//
// Its first version divided the payload by connect + write + the two-second wait
// for the server's reply. A 159 ms transmission printed as 16.82 kbps, and that
// number cost a bench session: it read as a slow link, and the link was fine.

#include <unity.h>
#include "throughput.h"

void setUp(void) {}
void tearDown(void) {}

static void test_bytes_over_milliseconds_is_kilobits_per_second(void)
{
    // 4538 B in 159 ms = 228.3 kbps. The same bytes over the 2159 ms that
    // included the reply wait read as 16.8 — the bug this pins.
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 228.3f, kbps_from(4538, 159));
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 16.82f, kbps_from(4538, 2159));
}

static void test_a_span_of_zero_is_not_infinite_speed(void)
{
    TEST_ASSERT_EQUAL_FLOAT(0.0f, kbps_from(4538, 0));
}

static void test_nothing_sent_is_zero_not_a_division_artefact(void)
{
    TEST_ASSERT_EQUAL_FLOAT(0.0f, kbps_from(0, 100));
}

static int run_all(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bytes_over_milliseconds_is_kilobits_per_second);
    RUN_TEST(test_a_span_of_zero_is_not_infinite_speed);
    RUN_TEST(test_nothing_sent_is_zero_not_a_division_artefact);
    return UNITY_END();
}

#ifdef ARDUINO
#include <Arduino.h>
void setup() { delay(2000); run_all(); }
void loop() {}
#else
int main(void) { return run_all(); }
#endif
