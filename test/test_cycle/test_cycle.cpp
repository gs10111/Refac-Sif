// Round 9 — A20, A21, A23 — the cycle itself, finally reachable from a host build.
//
// One guarantee has been structural since round 6 and written down as untested
// every time: runCycleIteration turns the radio off on EVERY path. That is the
// exact shape of R7 — the refactor called disconnect() only inside the
// successful-connect branch, so a failed connect left the radio powered while the
// device went on to acquire at 10 MHz.
//
// Reading three lines and believing them is what let R7 ship in the first place.
//
// TWO NEW SEAMS, and they are new for a reason that did not apply before. Every
// earlier unit RECEIVED `now` as a parameter, which is why BeltTrigger and
// AcquisitionService needed no clock. The loop is where those values are PRODUCED —
// millis() and digitalRead() enter the system here — so this is the one place a
// clock seam earns itself rather than being ceremony.

#include <unity.h>
#include <stdint.h>
#include <string.h>

#include "packet.h"
#include "ring_buffer.h"
#include "belt_trigger.h"
#include "imu.h"
#include "acquisition.h"
#include "radio.h"
#include "cpu.h"
#include "sleeper.h"
#include "transport.h"
#include "key_value_store.h"
#include "clock.h"
#include "trigger_source.h"
#include "battery_sense.h"
#include "power_manager.h"
#include "cycle_runner.h"

void setUp(void) {}
void tearDown(void) {}

// --- shared call log ------------------------------------------------------
static const uint32_t kMaxCalls = 64;

class CallLog
{
public:
    CallLog() : _count(0) {}
    void record(const char *tag)
    {
        if (_count < kMaxCalls)
            _tags[_count++] = tag;
    }
    bool contains(const char *tag) const
    {
        for (uint32_t i = 0; i < _count; i++)
            if (strcmp(_tags[i], tag) == 0)
                return true;
        return false;
    }
    uint32_t countOf(const char *tag) const
    {
        uint32_t n = 0;
        for (uint32_t i = 0; i < _count; i++)
            if (strcmp(_tags[i], tag) == 0)
                n++;
        return n;
    }

private:
    const char *_tags[kMaxCalls];
    uint32_t _count;
};

// --- fakes ----------------------------------------------------------------
class SilentImu : public IImu
{
public:
    void wake() override {}
    void setSamplingCode(uint8_t) override {}
    void sleep() override {}
    ImuStatus readSensorFrame(uint8_t *out) override
    {
        for (uint32_t i = 0; i < IMU_SENSOR_BYTES; i++)
            out[i] = 0x5A;
        return IMU_OK;
    }
};

class LoggingRadio : public IRadio
{
public:
    LoggingRadio(CallLog &log, bool connects) : _log(log), _connects(connects) {}
    bool connect(const char *, const char *, uint32_t) override
    {
        _log.record("radio.connect");
        return _connects;
    }
    void off() override { _log.record("radio.off"); }
    bool isConnected() override { return _connects; }

private:
    CallLog &_log;
    bool _connects;
};

// A transport whose config reply carries a chosen rate, so a test can watch what
// the cycle does with a rate that differs from the one already stored.
class RatedTransport : public ITransport
{
public:
    RatedTransport(uint16_t samplingCode) : _code(samplingCode) {}
    bool open(const char *, uint16_t) override { return true; }
    uint32_t write(const uint8_t *, uint32_t len) override { return len; }
    uint32_t readExact(uint8_t *out, uint32_t len, uint32_t) override
    {
        const uint8_t reply[SERVER_CONFIG_WIRE_BYTES] = {
            0xF0, 0x00, 0x14, 0x00, 0x05, 0x00, 0x05, 0x00, 0x00, 0x00,
            (uint8_t)(_code & 0xFF), (uint8_t)(_code >> 8)};
        uint32_t n = (len < SERVER_CONFIG_WIRE_BYTES) ? len : SERVER_CONFIG_WIRE_BYTES;
        for (uint32_t i = 0; i < n; i++)
            out[i] = reply[i];
        return n;
    }
    void close() override {}

private:
    uint16_t _code;
};

class QuietCpu : public ICpu
{
public:
    void setFrequencyMhz(uint32_t) override {}
    void delayMs(uint32_t) override {}
};

class QuietSleeper : public ISleeper
{
public:
    void enableTimerWakeup(uint64_t) override {}
    void enableExt0Wakeup(uint8_t, uint8_t) override {}
    void start() override {}
};

class LoggingTransport : public ITransport
{
public:
    LoggingTransport(CallLog &log) : _log(log), _bytes(0) {}
    bool open(const char *, uint16_t) override
    {
        _log.record("tcp.open");
        return true;
    }
    uint32_t write(const uint8_t *, uint32_t len) override
    {
        _bytes += len;
        _log.record("tcp.write");
        return len;
    }
    uint32_t readExact(uint8_t *out, uint32_t len, uint32_t) override
    {
        // A well-formed response with update = 0.
        static const uint8_t reply[SERVER_CONFIG_WIRE_BYTES] = {
            0xF0, 0x00, 0x14, 0x00, 0x05, 0x00, 0x05, 0x00, 0x00, 0x00};
        uint32_t n = (len < SERVER_CONFIG_WIRE_BYTES) ? len : SERVER_CONFIG_WIRE_BYTES;
        for (uint32_t i = 0; i < n; i++)
            out[i] = reply[i];
        return n;
    }
    void close() override { _log.record("tcp.close"); }
    uint32_t bytesWritten() const { return _bytes; }

private:
    CallLog &_log;
    uint32_t _bytes;
};

class MemoryStore : public IKeyValueStore
{
public:
    MemoryStore() : _value(false), _ushort(DEFAULT_SAMPLING_CODE), _ushortWrites(0) {}
    bool getBool(const char *, bool) override { return _value; }
    void putBool(const char *, bool value) override { _value = value; }
    uint16_t getUShort(const char *, uint16_t) override { return _ushort; }
    void putUShort(const char *, uint16_t value) override
    {
        _ushort = value;
        _ushortWrites++;
    }

    void seedUShort(uint16_t value) { _ushort = value; }
    uint16_t ushort() const { return _ushort; }
    uint32_t ushortWrites() const { return _ushortWrites; }

private:
    bool _value;
    uint16_t _ushort;
    uint32_t _ushortWrites;
};

// Advances by a fixed step on every read, so a test can drive the acquisition loop
// forward without knowing how many times it polls.
class SteppingClock : public IClock
{
public:
    explicit SteppingClock(uint32_t step) : _now(0), _step(step), _first(true) {}
    uint32_t millis() override
    {
        if (_first)
        {
            _first = false;
            return _now;
        }
        _now += _step;
        return _now;
    }

private:
    uint32_t _now;
    uint32_t _step;
    bool _first;
};

// Reads HIGH until told to fall, which is what ends an acquisition.
class ScriptedPin : public ITriggerSource
{
public:
    explicit ScriptedPin(uint32_t lowAfterReads)
        : _reads(0), _lowAfter(lowAfterReads) {}
    TriggerLevel read() override
    {
        return (++_reads > _lowAfter) ? TRIGGER_LEVEL_LOW : TRIGGER_LEVEL_HIGH;
    }

private:
    uint32_t _reads;
    uint32_t _lowAfter;
};

class FixedBattery : public IBatterySense
{
public:
    uint16_t readMv() override { return 3700; }
};

// --- fixture --------------------------------------------------------------
static const uint32_t kFrames = 8;

// ScriptedPin threshold. It drives the fixture AND the expected byte count, so the
// two move together by construction — change it to 5 and both follow.
//
// The prose below is the part that cannot be derived: a successful iteration stores
// kHighReads frames, NOT kHighReads + 1. The pin falls on read kHighReads + 1, and
// on that iteration acquisition.cpp:35 returns ACQ_STOPPED_BY_TRIGGER before storing
// anything — T27's contract, matching production's `while (!stopFlag)` test at the
// top of the sampling loop (ICM42688P.cpp:364). That fact is not recoverable from
// this file without tracing, so it is written down; the number is not.
static const uint32_t kHighReads = 3;
static uint8_t storage[kFrames * SAMPLE_SIZE_BYTES];

static NetworkConfig network(void)
{
    NetworkConfig net;
    net.ssid = "ssid";
    net.password = "password";
    net.host = "192.168.1.100";
    net.port = 12345;
    return net;
}

// A20 — the ordinary path. The radio comes up, the data goes out, and the radio is
// off again before the next acquisition starts at 10 MHz.
static void test_the_radio_is_turned_off_after_a_successful_iteration(void)
{
    CallLog log;
    SilentImu imu;
    RingBuffer ring(storage, kFrames, SAMPLE_SIZE_BYTES);
    AcquisitionService acq(imu, ring);
    acq.setConfig(default_server_config());
    BeltTrigger trigger;
    trigger.begin(0, 0); // no cooldown, so the first falling edge stops it
    LoggingRadio radio(log, true);
    QuietCpu cpu;
    QuietSleeper sleeper;
    PowerManager power(imu, radio, cpu, sleeper);
    LoggingTransport transport(log);
    MemoryStore store;
    SteppingClock clock(1);
    ScriptedPin pin(kHighReads);
    FixedBattery battery;

    CycleRunner runner(acq, trigger, ring, power, radio, transport, store, pin, battery, clock);
    ServerConfig config = default_server_config();

    CycleOutcome outcome = runner.runIteration(network(), config);

    TEST_ASSERT_FALSE(outcome.idleTimedOut);
    TEST_ASSERT_TRUE(log.contains("tcp.write"));
    // Header + every frame the ring held + battery. Derived from kHighReads,
    // not from a number read off a previous run.
    //
    // This assertion is stronger than the bool it replaced on DETECTION and weaker
    // on STABILITY — a bool cannot see a truncated payload, and a byte count moves
    // when the fixture does. The trade is worth it because the failure modes are not
    // symmetric: a bool fails FALSE NEGATIVE, silently, until an incident; a count
    // fails FALSE POSITIVE, loudly, on the next run. Do not swap it back for a bool
    // the next time it breaks.
    TEST_ASSERT_EQUAL_UINT32(HEADER_SIZE_BYTES + kHighReads * SAMPLE_SIZE_BYTES +
                                 BATTERY_SIZE_BYTES,
                             transport.bytesWritten());
    TEST_ASSERT_TRUE(log.contains("radio.off"));
}

// A21 — R7 verbatim. The connect fails, so there is nothing to disconnect, and the
// radio must go off anyway. The refactor's disconnect() lived inside the success
// branch and this path left the radio powered through the whole next acquisition,
// at roughly 80-100 mA against a 15 mA budget.
static void test_the_radio_is_turned_off_when_the_connection_fails(void)
{
    CallLog log;
    SilentImu imu;
    RingBuffer ring(storage, kFrames, SAMPLE_SIZE_BYTES);
    AcquisitionService acq(imu, ring);
    acq.setConfig(default_server_config());
    BeltTrigger trigger;
    trigger.begin(0, 0);
    LoggingRadio radio(log, false); // never connects
    QuietCpu cpu;
    QuietSleeper sleeper;
    PowerManager power(imu, radio, cpu, sleeper);
    LoggingTransport transport(log);
    MemoryStore store;
    SteppingClock clock(1);
    ScriptedPin pin(kHighReads);
    FixedBattery battery;

    CycleRunner runner(acq, trigger, ring, power, radio, transport, store, pin, battery, clock);
    ServerConfig config = default_server_config();

    CycleOutcome outcome = runner.runIteration(network(), config);

    TEST_ASSERT_FALSE(log.contains("tcp.open"));
    TEST_ASSERT_EQUAL_UINT32(0, transport.bytesWritten());
    TEST_ASSERT_TRUE(log.contains("radio.off"));
}

// A23 — the belt stopped. Production checks the flag before the transmit block and
// sleeps out of loop() (main.cpp:169-174), so nothing is sent — and the radio still
// has to end up off, because this path returns early and it is the one a reader
// most easily misses.
static void test_an_idle_timeout_turns_the_radio_off_and_transmits_nothing(void)
{
    CallLog log;
    SilentImu imu;
    RingBuffer ring(storage, kFrames, SAMPLE_SIZE_BYTES);
    AcquisitionService acq(imu, ring);
    acq.setConfig(default_server_config());
    BeltTrigger trigger;
    trigger.begin(0, 0);
    LoggingRadio radio(log, true);
    QuietCpu cpu;
    QuietSleeper sleeper;
    PowerManager power(imu, radio, cpu, sleeper);
    LoggingTransport transport(log);
    MemoryStore store;
    SteppingClock clock(700000); // two steps clear the 20-minute timeout
    ScriptedPin pin(1000);       // the magnet never comes round
    FixedBattery battery;

    CycleRunner runner(acq, trigger, ring, power, radio, transport, store, pin, battery, clock);
    ServerConfig config = default_server_config();

    CycleOutcome outcome = runner.runIteration(network(), config);

    TEST_ASSERT_TRUE(outcome.idleTimedOut);
    TEST_ASSERT_FALSE(log.contains("radio.connect"));
    TEST_ASSERT_FALSE(log.contains("tcp.open"));
    TEST_ASSERT_TRUE(log.contains("radio.off"));
    TEST_ASSERT_EQUAL_UINT32(0, transport.bytesWritten());
}

// ---------------------------------------------------------------------------
// Sampling rate — the server picks it, the device stores it, the next boot runs it
// ---------------------------------------------------------------------------

// Builds the whole cycle around a transport whose reply carries `sentCode`, with
// `storedCode` already in NVS, and runs one iteration.
static void run_cycle_with_rate(uint16_t storedCode, uint16_t sentCode,
                                MemoryStore &store, ServerConfig &config)
{
    CallLog log;
    SilentImu imu;
    RingBuffer ring(storage, kFrames, SAMPLE_SIZE_BYTES);
    AcquisitionService acq(imu, ring);
    acq.setConfig(default_server_config());
    BeltTrigger trigger;
    trigger.begin(0, 0);
    LoggingRadio radio(log, true);
    QuietCpu cpu;
    QuietSleeper sleeper;
    PowerManager power(imu, radio, cpu, sleeper);
    RatedTransport transport(sentCode);
    SteppingClock clock(1);
    ScriptedPin pin(kHighReads);
    FixedBattery battery;

    store.seedUShort(storedCode);

    CycleRunner runner(acq, trigger, ring, power, radio, transport, store, pin,
                       battery, clock);
    runner.runIteration(network(), config);
}

static void test_a_new_rate_from_the_server_is_written_to_nvs(void)
{
    MemoryStore store;
    ServerConfig config = default_server_config();

    run_cycle_with_rate(SAMPLING_CODE_50HZ, SAMPLING_CODE_200HZ, store, config);

    TEST_ASSERT_EQUAL_UINT16(SAMPLING_CODE_200HZ, store.ushort());
    TEST_ASSERT_EQUAL_UINT32(1, store.ushortWrites());
    TEST_ASSERT_EQUAL_UINT16(SAMPLING_CODE_200HZ, config.sampling_code);
}

static void test_the_rate_already_stored_is_not_written_again(void)
{
    // Every capture answers with the same rate. Writing it each time would burn
    // the flash for no change at all.
    MemoryStore store;
    ServerConfig config = default_server_config();

    run_cycle_with_rate(SAMPLING_CODE_50HZ, SAMPLING_CODE_50HZ, store, config);

    TEST_ASSERT_EQUAL_UINT32(0, store.ushortWrites());
}

static void test_a_server_with_no_opinion_leaves_the_stored_rate_alone(void)
{
    // Code 0 is a server that does not configure rates — win_server, which
    // exists to flash firmware. It must not re-rate the device to nothing.
    MemoryStore store;
    ServerConfig config = default_server_config();
    config.sampling_code = SAMPLING_CODE_200HZ;

    run_cycle_with_rate(SAMPLING_CODE_200HZ, 0, store, config);

    TEST_ASSERT_EQUAL_UINT32(0, store.ushortWrites());
    TEST_ASSERT_EQUAL_UINT16(SAMPLING_CODE_200HZ, store.ushort());
    TEST_ASSERT_EQUAL_UINT16(SAMPLING_CODE_200HZ, config.sampling_code);
}

static void test_a_reserved_nibble_from_the_server_is_refused(void)
{
    // 13 is Reserved for the gyroscope. Written to NVS it would be applied at
    // the next boot, configuring the part into an undefined mode.
    MemoryStore store;
    ServerConfig config = default_server_config();

    run_cycle_with_rate(SAMPLING_CODE_50HZ, 13, store, config);

    TEST_ASSERT_EQUAL_UINT32(0, store.ushortWrites());
    TEST_ASSERT_EQUAL_UINT16(SAMPLING_CODE_50HZ, store.ushort());
    TEST_ASSERT_EQUAL_UINT16(SAMPLING_CODE_50HZ, config.sampling_code);
}

static int run_all(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_radio_is_turned_off_after_a_successful_iteration);
    RUN_TEST(test_the_radio_is_turned_off_when_the_connection_fails);
    RUN_TEST(test_an_idle_timeout_turns_the_radio_off_and_transmits_nothing);
    RUN_TEST(test_a_new_rate_from_the_server_is_written_to_nvs);
    RUN_TEST(test_the_rate_already_stored_is_not_written_again);
    RUN_TEST(test_a_server_with_no_opinion_leaves_the_stored_rate_alone);
    RUN_TEST(test_a_reserved_nibble_from_the_server_is_refused);
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
