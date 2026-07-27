// Round 6 — T37..T42 — the power path, and the two regressions that were only
// ever going to be caught by asserting ORDER.
//
//   R5  nothing ever called imu.sleep(). ICM42688P::sleep() existed with no
//       caller, so PWR_MGMT0 stayed in Low Noise — accel and gyro both running,
//       ~1.2 mA — through every 240-minute deep sleep, against a ~15 uA target.
//       Production calls it on every sleep path: main.cpp:141, :149, :170.
//   R7  the radio was left powered through the acquisition. WiFi.config() enables
//       STA on arduino-esp32, and nothing turned it off before collecting.
//       Production turns it off unconditionally at main.cpp:138 and :300-301.
//
// Both were on my hardware-only list. They are not: they are ORDERING properties,
// and ordering is assertable the moment the collaborators record. That is the whole
// reason PowerManager takes IImu, IRadio, ICpu and ISleeper instead of calling
// esp_* directly — the seam is not for isolation, it is because "before" is the
// thing under test.

#include <unity.h>
#include <stdint.h>
#include <string.h>

#include "packet.h"
#include "board.h"
#include "imu.h"
#include "radio.h"
#include "cpu.h"
#include "sleeper.h"
#include "power_manager.h"
#include "battery.h"

void setUp(void) {}
void tearDown(void) {}

// --- shared call log ------------------------------------------------------
static const uint32_t kMaxCalls = 32;

class CallLog
{
public:
    CallLog() : _count(0) {}

    void record(const char *tag)
    {
        if (_count < kMaxCalls)
            _tags[_count++] = tag;
    }

    uint32_t count() const { return _count; }
    const char *at(uint32_t i) const { return i < _count ? _tags[i] : "<past end>"; }

    bool contains(const char *tag) const
    {
        for (uint32_t i = 0; i < _count; i++)
            if (strcmp(_tags[i], tag) == 0)
                return true;
        return false;
    }

private:
    const char *_tags[kMaxCalls];
    uint32_t _count;
};

static void assert_sequence(const CallLog &log, const char *const *expected, uint32_t n)
{
    TEST_ASSERT_EQUAL_UINT32(n, log.count());
    for (uint32_t i = 0; i < n; i++)
        TEST_ASSERT_EQUAL_STRING(expected[i], log.at(i));
}

// --- fakes ----------------------------------------------------------------
class LoggingImu : public IImu
{
public:
    explicit LoggingImu(CallLog &log) : _log(log) {}
    void wake() override { _log.record("imu.wake"); }
    void sleep() override { _log.record("imu.sleep"); }
    ImuStatus readSensorFrame(uint8_t *) override { return IMU_NOT_READY; }

private:
    CallLog &_log;
};

class LoggingRadio : public IRadio
{
public:
    LoggingRadio(CallLog &log, bool connected) : _log(log), _connected(connected) {}
    bool connect(const char *, const char *, uint32_t) override
    {
        _log.record("radio.connect");
        return _connected;
    }
    void off() override { _log.record("radio.off"); }
    bool isConnected() override { return _connected; }

private:
    CallLog &_log;
    bool _connected;
};

class LoggingCpu : public ICpu
{
public:
    explicit LoggingCpu(CallLog &log) : _log(log), _lastDelayMs(0) {}

    void setFrequencyMhz(uint32_t mhz) override
    {
        if (mhz == 10)
            _log.record("cpu.10");
        else if (mhz == 240)
            _log.record("cpu.240");
        else
            _log.record("cpu.other");
    }

    void delayMs(uint32_t ms) override
    {
        _lastDelayMs = ms;
        _log.record("cpu.delay");
    }

    uint32_t lastDelayMs() const { return _lastDelayMs; }

private:
    CallLog &_log;
    uint32_t _lastDelayMs;
};

class LoggingSleeper : public ISleeper
{
public:
    explicit LoggingSleeper(CallLog &log) : _log(log), _us(0), _pin(0), _level(1) {}

    void enableTimerWakeup(uint64_t us) override
    {
        _us = us;
        _log.record("sleep.timer");
    }

    void enableExt0Wakeup(uint8_t pin, uint8_t level) override
    {
        _pin = pin;
        _level = level;
        _log.record("sleep.ext0");
    }

    // Returns, unlike the device, where esp_deep_sleep_start never comes back.
    void start() override { _log.record("sleep.start"); }

    uint64_t microseconds() const { return _us; }
    uint8_t pin() const { return _pin; }
    uint8_t level() const { return _level; }

private:
    CallLog &_log;
    uint64_t _us;
    uint8_t _pin;
    uint8_t _level;
};

// T37 — R5, the timer path. Production turns the radio off (main.cpp:138) and then
// sleeps the IMU (:141) before arming the wakeup.
static void test_timer_sleep_turns_the_radio_off_and_sleeps_the_imu_first(void)
{
    CallLog log;
    LoggingImu imu(log);
    LoggingRadio radio(log, false);
    LoggingCpu cpu(log);
    LoggingSleeper sleeper(log);
    PowerManager power(imu, radio, cpu, sleeper);

    power.deepSleepTimer(DEFAULT_SLEEP_TIME_MIN);

    static const char *const expected[] = {
        "radio.off", "imu.sleep", "sleep.timer", "sleep.start"};
    assert_sequence(log, expected, 4);
}

// T38 — R5, the trigger path. The device waits here for hours with the belt idle,
// so an IMU left in Low Noise costs exactly as much as it does on the timer path.
static void test_trigger_sleep_turns_the_radio_off_and_sleeps_the_imu_first(void)
{
    CallLog log;
    LoggingImu imu(log);
    LoggingRadio radio(log, false);
    LoggingCpu cpu(log);
    LoggingSleeper sleeper(log);
    PowerManager power(imu, radio, cpu, sleeper);

    power.deepSleepUntilTrigger(MAGNET_TRIGGER_PIN);

    static const char *const expected[] = {
        "radio.off", "imu.sleep", "sleep.ext0", "sleep.start"};
    assert_sequence(log, expected, 4);

    TEST_ASSERT_EQUAL_UINT8(MAGNET_TRIGGER_PIN, sleeper.pin());
    TEST_ASSERT_EQUAL_UINT8(0, sleeper.level()); // LOW wakes, main.cpp:151
}

// T39 — R7. The radio goes off BEFORE the clock drops, not after: the WiFi stack
// was never meant to run at 10 MHz, and an acquisition with the radio up draws
// ~80-100 mA instead of ~15.
static void test_entering_acquisition_mode_turns_the_radio_off_then_drops_to_10mhz(void)
{
    CallLog log;
    LoggingImu imu(log);
    LoggingRadio radio(log, true);
    LoggingCpu cpu(log);
    LoggingSleeper sleeper(log);
    PowerManager power(imu, radio, cpu, sleeper);

    power.enterAcquisitionMode();

    static const char *const expected[] = {"radio.off", "cpu.10"};
    assert_sequence(log, expected, 2);
}

// T40 — R7's actual regression shape. The refactor called disconnect() only inside
// the successful-connect branch, so a failed connect left the radio powered
// (app.cpp:51-65 before this round). Production turns it off unconditionally at
// main.cpp:300-301, on every path through loop().
//
// NOTE: this is the reframed T40. The original wording — "radio is off on the
// connect-failure path" — is a property of the App's cycle, which is Arduino code
// and not reachable from a host build. What IS testable, and is the half that
// carries the guarantee, is that turning the radio off never depends on its state.
// The App side is then structural: one unconditional call, outside the if.
static void test_entering_acquisition_mode_turns_the_radio_off_even_when_not_connected(void)
{
    CallLog log;
    LoggingImu imu(log);
    LoggingRadio radio(log, false); // connect failed; nothing to disconnect
    LoggingCpu cpu(log);
    LoggingSleeper sleeper(log);
    PowerManager power(imu, radio, cpu, sleeper);

    power.enterAcquisitionMode();

    TEST_ASSERT_TRUE(log.contains("radio.off"));
}

// T41 — DEC-10. Production raises the clock and then waits 100 ms before bringing
// WiFi up (main.cpp:176-178). The refactor dropped the settle delay.
static void test_entering_transmit_mode_raises_to_240mhz_then_settles(void)
{
    CallLog log;
    LoggingImu imu(log);
    LoggingRadio radio(log, true);
    LoggingCpu cpu(log);
    LoggingSleeper sleeper(log);
    PowerManager power(imu, radio, cpu, sleeper);

    power.enterTransmitMode();

    static const char *const expected[] = {"cpu.240", "cpu.delay"};
    assert_sequence(log, expected, 2);
    TEST_ASSERT_EQUAL_UINT32(100, cpu.lastDelayMs());
}

// T42 — minutes to microseconds, in 64 bits. 240 minutes is 14,400,000,000 us,
// which overflows a uint32_t at 32 bits: a 32-bit computation yields 1,814,065,152
// us, about 30 minutes, and the device would wake eight times too often for the
// rest of its life. Production casts to uint64 (main.cpp:173).
static void test_timer_sleep_duration_is_sleep_min_times_60_million_microseconds(void)
{
    CallLog log;
    LoggingImu imu(log);
    LoggingRadio radio(log, false);
    LoggingCpu cpu(log);
    LoggingSleeper sleeper(log);
    PowerManager power(imu, radio, cpu, sleeper);

    power.deepSleepTimer(DEFAULT_SLEEP_TIME_MIN);

    TEST_ASSERT_EQUAL_UINT64(14400000000ULL, sleeper.microseconds());
}

// T49 — the battery conversion, pulled out of the Arduino layer so it can be
// pinned. Production uses map(raw, 0, 4095, 0, 19803) at main.cpp:259-260. An
// integer map from a zero origin is exactly multiply-then-divide, so the arithmetic
// is equivalent — but that equivalence was verified by reading, and this is what
// makes it verified by running.
static void test_battery_conversion_matches_the_production_map(void)
{
    TEST_ASSERT_EQUAL_UINT16(0, battery_mv_from_raw(0));
    TEST_ASSERT_EQUAL_UINT16(19803, battery_mv_from_raw(4095));
    TEST_ASSERT_EQUAL_UINT16(9899, battery_mv_from_raw(2047));
    TEST_ASSERT_EQUAL_UINT16(4831, battery_mv_from_raw(999));
}

static int run_all(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_battery_conversion_matches_the_production_map);
    RUN_TEST(test_timer_sleep_turns_the_radio_off_and_sleeps_the_imu_first);
    RUN_TEST(test_trigger_sleep_turns_the_radio_off_and_sleeps_the_imu_first);
    RUN_TEST(test_entering_acquisition_mode_turns_the_radio_off_then_drops_to_10mhz);
    RUN_TEST(test_entering_acquisition_mode_turns_the_radio_off_even_when_not_connected);
    RUN_TEST(test_entering_transmit_mode_raises_to_240mhz_then_settles);
    RUN_TEST(test_timer_sleep_duration_is_sleep_min_times_60_million_microseconds);
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
