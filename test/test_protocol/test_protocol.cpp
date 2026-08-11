// Round 1 — T01..T05 — wire contract, server -> ESP32.
//
// Contract (lead-owned, do not change without telling the lead):
//   12 bytes, 6 x uint16 little-endian
//     offset  0: sleep_min     (default 240)
//     offset  2: idle_min      (default  20)
//     offset  4: max_acq       (default   5)
//     offset  6: cooldown_sec  (default   5)
//     offset  8: update        (default   0)
//     offset 10: sampling_code (default   9 = 50 Hz)
//
// The first ten bytes are the layout pyFiles/win_server.py packs with '<HHHHH',
// at the same offsets. The rate was appended in 2026-08, so a server that still
// sends ten bytes no longer configures this firmware at all: the frame is
// refused whole and the device keeps the config it is running.

#include <unity.h>
#include <stddef.h>
#include <stdint.h>

#include "packet.h"

void setUp(void) {}
void tearDown(void) {}

// T01 — the refactor shrank ServerConfig to 8 bytes by dropping `update`, so the
// firmware left 2 bytes unread in the socket and OTA could never be armed.
static void test_server_config_is_10_bytes(void)
{
    // The five original fields still occupy the first ten bytes; the rate was
    // appended after them. See test_server_config_is_12_bytes_with_the_rate.
    TEST_ASSERT_EQUAL_UINT32(10, (uint32_t)offsetof(ServerConfig, sampling_code));
}

// T02 — field order is the wire order. A struct that is 10 bytes but reordered
// would still decode garbage, so pin the offsets, not just the size.
static void test_server_config_field_offsets_are_0_2_4_6_8(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)offsetof(ServerConfig, sleep_time_min));
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)offsetof(ServerConfig, idle_timeout_min));
    TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)offsetof(ServerConfig, max_acquisitions));
    TEST_ASSERT_EQUAL_UINT32(6, (uint32_t)offsetof(ServerConfig, trigger_cooldown_sec));
    TEST_ASSERT_EQUAL_UINT32(8, (uint32_t)offsetof(ServerConfig, update));
}

// T03 — golden blob: 240, 20, 5, 5, 1 packed as '<HHHHH'.
static void test_parse_config_from_golden_le_blob_240_20_5_5_1(void)
{
    const uint8_t blob[12] = {
        0xF0, 0x00,  // 240
        0x14, 0x00,  //  20
        0x05, 0x00,  //   5
        0x05, 0x00,  //   5
        0x01, 0x00,  //   1
        0x09, 0x00,  //   9 — 50 Hz, the rate this golden frame has always meant
    };

    ServerConfig cfg;
    TEST_ASSERT_TRUE(parse_server_config(blob, (uint32_t)sizeof(blob), cfg));

    TEST_ASSERT_EQUAL_UINT16(240, cfg.sleep_time_min);
    TEST_ASSERT_EQUAL_UINT16(20, cfg.idle_timeout_min);
    TEST_ASSERT_EQUAL_UINT16(5, cfg.max_acquisitions);
    TEST_ASSERT_EQUAL_UINT16(5, cfg.trigger_cooldown_sec);
    TEST_ASSERT_EQUAL_UINT16(1, cfg.update);
}

// T04 — "the read must require 10 bytes". A short frame must be rejected whole:
// no partial application, or a truncated response would silently reconfigure the
// duty cycle with half-parsed values.
static void test_parse_config_rejects_frame_shorter_than_10_bytes(void)
{
    const uint8_t shortBlob[9] = {
        0xF0, 0x00, 0x14, 0x00, 0x05, 0x00, 0x05, 0x00, 0x01,
    };

    ServerConfig cfg = default_server_config();
    TEST_ASSERT_FALSE(parse_server_config(shortBlob, (uint32_t)sizeof(shortBlob), cfg));

    TEST_ASSERT_EQUAL_UINT16(DEFAULT_SLEEP_TIME_MIN, cfg.sleep_time_min);
    TEST_ASSERT_EQUAL_UINT16(DEFAULT_IDLE_TIMEOUT_MIN, cfg.idle_timeout_min);
    TEST_ASSERT_EQUAL_UINT16(DEFAULT_MAX_ACQUISITIONS, cfg.max_acquisitions);
    TEST_ASSERT_EQUAL_UINT16(DEFAULT_TRIGGER_COOLDOWN_SEC, cfg.trigger_cooldown_sec);
    TEST_ASSERT_EQUAL_UINT16(DEFAULT_UPDATE, cfg.update);
}

// T05 — defaults used before the first server contact: 240 / 20 / 5 / 5 / 0.
static void test_defaults_are_240_20_5_5_0(void)
{
    ServerConfig cfg = default_server_config();

    TEST_ASSERT_EQUAL_UINT16(240, cfg.sleep_time_min);
    TEST_ASSERT_EQUAL_UINT16(20, cfg.idle_timeout_min);
    TEST_ASSERT_EQUAL_UINT16(5, cfg.max_acquisitions);
    TEST_ASSERT_EQUAL_UINT16(5, cfg.trigger_cooldown_sec);
    TEST_ASSERT_EQUAL_UINT16(0, cfg.update);
}

// ---------------------------------------------------------------------------
// Sampling rate — the sixth field, appended when the rate became configurable
// ---------------------------------------------------------------------------

static void test_server_config_is_12_bytes_with_the_rate(void)
{
    TEST_ASSERT_EQUAL_UINT32(12, (uint32_t)SERVER_CONFIG_WIRE_BYTES);
    TEST_ASSERT_EQUAL_UINT32(12, (uint32_t)sizeof(ServerConfig));
}

static void test_parse_config_reads_the_sampling_code_from_bytes_10_and_11(void)
{
    // 240, 20, 5, 5, 0, 7 — the last field is the ODR nibble for 200 Hz.
    const uint8_t blob[12] = {0xF0, 0x00, 0x14, 0x00, 0x05, 0x00,
                              0x05, 0x00, 0x00, 0x00, 0x07, 0x00};
    ServerConfig cfg = default_server_config();

    TEST_ASSERT_TRUE(parse_server_config(blob, sizeof(blob), cfg));
    TEST_ASSERT_EQUAL_UINT16(7, cfg.sampling_code);
    TEST_ASSERT_EQUAL_UINT16(240, cfg.sleep_time_min);
}

static void test_parse_config_rejects_the_legacy_ten_byte_frame(void)
{
    // A server that predates the rate field leaves the whole config untouched
    // rather than half-applying it: the device keeps running what it has.
    const uint8_t legacy[10] = {0xF0, 0x00, 0x14, 0x00, 0x05, 0x00,
                                0x05, 0x00, 0x01, 0x00};
    ServerConfig cfg = default_server_config();
    cfg.sleep_time_min = 111;

    TEST_ASSERT_FALSE(parse_server_config(legacy, sizeof(legacy), cfg));
    TEST_ASSERT_EQUAL_UINT16(111, cfg.sleep_time_min);
    TEST_ASSERT_EQUAL_UINT16(0, cfg.update);
}

static void test_default_rate_is_the_fifty_hertz_the_fleet_runs(void)
{
    ServerConfig cfg = default_server_config();

    TEST_ASSERT_EQUAL_UINT16(SAMPLING_CODE_50HZ, cfg.sampling_code);
    TEST_ASSERT_EQUAL_UINT16(9, cfg.sampling_code);
}

static void test_only_the_five_rates_the_part_can_run_are_valid(void)
{
    // 7..11 = 200, 100, 50, 25, 12.5 Hz. Both sensors run at all five.
    TEST_ASSERT_TRUE(is_valid_sampling_code(7));
    TEST_ASSERT_TRUE(is_valid_sampling_code(8));
    TEST_ASSERT_TRUE(is_valid_sampling_code(9));
    TEST_ASSERT_TRUE(is_valid_sampling_code(10));
    TEST_ASSERT_TRUE(is_valid_sampling_code(11));
}

static void test_a_reserved_nibble_is_never_accepted_as_a_rate(void)
{
    // 12-14 are Reserved for the gyroscope on the ICM-42688-P; 0 is the
    // server's "no opinion"; 15 (500 Hz) is out of scope by decision.
    TEST_ASSERT_FALSE(is_valid_sampling_code(0));
    TEST_ASSERT_FALSE(is_valid_sampling_code(6));
    TEST_ASSERT_FALSE(is_valid_sampling_code(12));
    TEST_ASSERT_FALSE(is_valid_sampling_code(14));
    TEST_ASSERT_FALSE(is_valid_sampling_code(15));
    TEST_ASSERT_FALSE(is_valid_sampling_code(65535));
}

static void test_a_stored_rate_that_is_not_valid_falls_back_to_fifty(void)
{
    // What NVS returns is not trusted: a corrupt or never-written key must not
    // reach ACCEL_CONFIG0 as a Reserved nibble.
    TEST_ASSERT_EQUAL_UINT8(7, sampling_code_or_default(7));
    TEST_ASSERT_EQUAL_UINT8(SAMPLING_CODE_50HZ, sampling_code_or_default(0));
    TEST_ASSERT_EQUAL_UINT8(SAMPLING_CODE_50HZ, sampling_code_or_default(13));
    TEST_ASSERT_EQUAL_UINT8(SAMPLING_CODE_50HZ, sampling_code_or_default(65535));
}

static int run_all(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_server_config_is_10_bytes);
    RUN_TEST(test_server_config_field_offsets_are_0_2_4_6_8);
    RUN_TEST(test_parse_config_from_golden_le_blob_240_20_5_5_1);
    RUN_TEST(test_parse_config_rejects_frame_shorter_than_10_bytes);
    RUN_TEST(test_defaults_are_240_20_5_5_0);
    RUN_TEST(test_server_config_is_12_bytes_with_the_rate);
    RUN_TEST(test_parse_config_reads_the_sampling_code_from_bytes_10_and_11);
    RUN_TEST(test_parse_config_rejects_the_legacy_ten_byte_frame);
    RUN_TEST(test_default_rate_is_the_fifty_hertz_the_fleet_runs);
    RUN_TEST(test_only_the_five_rates_the_part_can_run_are_valid);
    RUN_TEST(test_a_reserved_nibble_is_never_accepted_as_a_rate);
    RUN_TEST(test_a_stored_rate_that_is_not_valid_falls_back_to_fifty);
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
