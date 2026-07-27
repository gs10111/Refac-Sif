// Round 1 — T01..T05 — wire contract, server -> ESP32.
//
// Contract (lead-owned, do not change without telling the lead):
//   10 bytes, 5 x uint16 little-endian
//     offset 0: sleep_min     (default 240)
//     offset 2: idle_min      (default  20)
//     offset 4: max_acq       (default   5)
//     offset 6: cooldown_sec  (default   5)
//     offset 8: update        (default   0)
//
// This layout keeps the legacy pyFiles/win_server.py wire-compatible with the
// new firmware — it packs '<HHHHH' with exactly these five fields in this order.

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
    TEST_ASSERT_EQUAL_UINT32(10, (uint32_t)sizeof(ServerConfig));
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
    const uint8_t blob[10] = {
        0xF0, 0x00,  // 240
        0x14, 0x00,  //  20
        0x05, 0x00,  //   5
        0x05, 0x00,  //   5
        0x01, 0x00,  //   1
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

static int run_all(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_server_config_is_10_bytes);
    RUN_TEST(test_server_config_field_offsets_are_0_2_4_6_8);
    RUN_TEST(test_parse_config_from_golden_le_blob_240_20_5_5_1);
    RUN_TEST(test_parse_config_rejects_frame_shorter_than_10_bytes);
    RUN_TEST(test_defaults_are_240_20_5_5_0);
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
