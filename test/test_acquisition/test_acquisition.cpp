// Round 4 — T27..T33 — one acquisition, and the counter that bounds a cycle.
//
// This is the round that makes R3 and R4 stop being true:
//   R3  nothing ever ended an acquisition, so every one ran the full 20-minute
//       idle timeout instead of one belt revolution
//   R4  the counter lived in a heap object rebuilt on every boot, so
//       max_acquisitions was unreachable
//
// It is also where the 16-byte misalignment leaves the live path: storage moves
// off the driver and onto the frame-addressed RingBuffer.
//
// SHAPE. collect() is a step function, not a blocking loop — step(now, trigger).
// Same reasoning that kept BeltTrigger free of an IClock: a call that already
// receives `now` has nothing left for a clock seam to abstract, and the tight
// polling loop belongs to the App, which is where the Arduino code lives.
//
// The driver returns the 14 SENSOR bytes; this service prepends the 4-byte
// timestamp. Production samples millis() BEFORE the SPI reads (ICM42688P.cpp:371,
// then :379), so the timestamp handed to step() is the one that lands in the
// frame — and the 18-byte wire layout ends up owned in exactly one place that a
// host test can reach.

#include <unity.h>
#include <stdint.h>
#include <string.h>

#include "packet.h"
#include "ring_buffer.h"
#include "belt_trigger.h"
#include "imu.h"
#include "acquisition.h"

void setUp(void) {}
void tearDown(void) {}

// --- fake IMU -------------------------------------------------------------
// Reports NOT_READY until told otherwise, and fills every frame with a marker
// byte so a stored frame can be identified from its contents alone.
class FakeImu : public IImu
{
public:
    FakeImu() : _ready(false), _marker(0), _wakeCalls(0), _sleepCalls(0) {}

    void wake() override { _wakeCalls++; }
    void setSamplingCode(uint8_t code) override { _samplingCode = code; }
    uint8_t samplingCode() const { return _samplingCode; }
    void sleep() override { _sleepCalls++; }

    ImuStatus readSensorFrame(uint8_t *out) override
    {
        if (!_ready)
            return IMU_NOT_READY;
        for (uint32_t i = 0; i < IMU_SENSOR_BYTES; i++)
            out[i] = (uint8_t)(_marker + i);
        return IMU_OK;
    }

    void setReady(bool ready) { _ready = ready; }
    void setMarker(uint8_t marker) { _marker = marker; }
    uint32_t wakeCalls() const { return _wakeCalls; }
    uint8_t _samplingCode = 0;
    uint32_t sleepCalls() const { return _sleepCalls; }

private:
    bool _ready;
    uint8_t _marker;
    uint32_t _wakeCalls;
    uint32_t _sleepCalls;
};

// --- fixture --------------------------------------------------------------
static const uint32_t kFrames = 8;
static uint8_t storage[kFrames * SAMPLE_SIZE_BYTES];

static const uint32_t kIdleTimeoutMs = (uint32_t)DEFAULT_IDLE_TIMEOUT_MIN * 60UL * 1000UL; // 1200000

static ServerConfig defaults(void)
{
    return default_server_config();
}

// T27 — the belt came round again. This is the contract the deleted buttonTask
// used to provide and that nothing has provided since.
static void test_collect_stops_when_trigger_requests_stop(void)
{
    FakeImu imu;
    RingBuffer ring(storage, kFrames, SAMPLE_SIZE_BYTES);
    AcquisitionService acq(imu, ring);
    acq.setConfig(defaults());

    acq.beginAcquisition(0);

    TEST_ASSERT_EQUAL_UINT8(ACQ_RUNNING, acq.step(1, TRIGGER_EVENT_NONE));
    TEST_ASSERT_EQUAL_UINT8(ACQ_RUNNING, acq.step(2, TRIGGER_EVENT_RELEASED));
    TEST_ASSERT_EQUAL_UINT8(ACQ_STOPPED_BY_TRIGGER, acq.step(3, TRIGGER_EVENT_STOP));
}

// T28 — the belt stopped. Production discards this acquisition unsent
// (main.cpp:169 checks the flag before the transmit block), so the outcome has to
// be distinguishable from a trigger stop.
static void test_collect_stops_after_the_idle_timeout_with_no_trigger(void)
{
    FakeImu imu;
    RingBuffer ring(storage, kFrames, SAMPLE_SIZE_BYTES);
    AcquisitionService acq(imu, ring);
    acq.setConfig(defaults());

    acq.beginAcquisition(0);

    TEST_ASSERT_EQUAL_UINT8(ACQ_STOPPED_BY_IDLE, acq.step(kIdleTimeoutMs + 1, TRIGGER_EVENT_NONE));
}

// T29 — and not one millisecond early. The middle probe is the one that matters:
// it sits past where a timeout counted in SECONDS would have fired.
static void test_collect_does_not_stop_before_the_idle_timeout(void)
{
    FakeImu imu;
    RingBuffer ring(storage, kFrames, SAMPLE_SIZE_BYTES);
    AcquisitionService acq(imu, ring);
    acq.setConfig(defaults());

    acq.beginAcquisition(0);

    TEST_ASSERT_EQUAL_UINT8(ACQ_RUNNING, acq.step(1, TRIGGER_EVENT_NONE));

    // 20 seconds. A unit error here is invisible to any test that only probes the
    // endpoints — it still stops, it still stops at a timeout, and the device
    // sleeps after 20 seconds instead of 20 minutes for the rest of its life.
    TEST_ASSERT_EQUAL_UINT8(ACQ_RUNNING, acq.step(20001, TRIGGER_EVENT_NONE));

    // Exactly on the boundary. Production compares with a strict `>`
    // (ICM42688P.cpp:436), so this one is still running.
    TEST_ASSERT_EQUAL_UINT8(ACQ_RUNNING, acq.step(kIdleTimeoutMs, TRIGGER_EVENT_NONE));
}

// T30 — the counter survives across acquisitions because nothing deep-sleeps
// between them any more. This is D1 seen from the service side.
static void test_acquisition_count_accumulates_across_consecutive_collects(void)
{
    FakeImu imu;
    RingBuffer ring(storage, kFrames, SAMPLE_SIZE_BYTES);
    AcquisitionService acq(imu, ring);
    acq.setConfig(defaults());

    TEST_ASSERT_EQUAL_UINT16(0, acq.acquisitionsAttempted());

    acq.beginAcquisition(0);
    TEST_ASSERT_EQUAL_UINT16(1, acq.acquisitionsAttempted());

    acq.beginAcquisition(10);
    TEST_ASSERT_EQUAL_UINT16(2, acq.acquisitionsAttempted());

    acq.beginAcquisition(20);
    TEST_ASSERT_EQUAL_UINT16(3, acq.acquisitionsAttempted());
}

// T31 — one acquisition is one increment, however many samples it takes. The
// counter bounds the CYCLE; a counter that moved per sample would end a cycle in
// milliseconds.
static void test_acquisition_count_is_not_advanced_by_steps(void)
{
    FakeImu imu;
    RingBuffer ring(storage, kFrames, SAMPLE_SIZE_BYTES);
    AcquisitionService acq(imu, ring);
    acq.setConfig(defaults());
    imu.setReady(true);

    acq.beginAcquisition(0);
    for (uint32_t t = 1; t <= 50; t++)
        acq.step(t, TRIGGER_EVENT_NONE);

    TEST_ASSERT_EQUAL_UINT16(1, acq.acquisitionsAttempted());
}

// T31b — attempts, not successes.
//
// The guarantee is structural: this service has no idea whether a transmit
// happened, so nothing downstream can suppress the increment. That is the point.
// Counting successes instead would leave a device with the server unreachable
// collecting and failing forever at full draw, never reaching max_acquisitions,
// never sleeping — with every happy-path test green while it happens.
static void test_acquisition_count_increments_even_when_the_transmit_fails(void)
{
    FakeImu imu;
    RingBuffer ring(storage, kFrames, SAMPLE_SIZE_BYTES);
    AcquisitionService acq(imu, ring);
    acq.setConfig(defaults());

    // Two acquisitions, and between them the transmit that never happened.
    acq.beginAcquisition(0);
    TEST_ASSERT_EQUAL_UINT8(ACQ_STOPPED_BY_TRIGGER, acq.step(1, TRIGGER_EVENT_STOP));

    acq.beginAcquisition(2);
    TEST_ASSERT_EQUAL_UINT8(ACQ_STOPPED_BY_TRIGGER, acq.step(3, TRIGGER_EVENT_STOP));

    TEST_ASSERT_EQUAL_UINT16(2, acq.acquisitionsAttempted());
}

// T31c — production resets the counter on BOTH break paths, ICM42688P.cpp:438 for
// the idle timeout and :445 for max_acq. A reset on only one of them is a leak
// that shows up on the SECOND cycle: a cycle ended by idle timeout would make the
// next one sleep early, and nobody reproduces that on a bench.
static void test_acquisition_count_resets_when_the_cycle_ends_by_either_path(void)
{
    FakeImu imu;
    RingBuffer ring(storage, kFrames, SAMPLE_SIZE_BYTES);
    AcquisitionService acq(imu, ring);
    acq.setConfig(defaults());

    // Ended by the counter reaching max.
    acq.beginAcquisition(0);
    acq.step(1, TRIGGER_EVENT_STOP);
    acq.beginAcquisition(2);
    acq.step(3, TRIGGER_EVENT_STOP);
    TEST_ASSERT_EQUAL_UINT16(2, acq.acquisitionsAttempted());
    acq.endCycle();
    TEST_ASSERT_EQUAL_UINT16(0, acq.acquisitionsAttempted());

    // Ended by the idle timeout.
    acq.beginAcquisition(0);
    TEST_ASSERT_EQUAL_UINT8(ACQ_STOPPED_BY_IDLE, acq.step(kIdleTimeoutMs + 1, TRIGGER_EVENT_NONE));
    TEST_ASSERT_EQUAL_UINT16(1, acq.acquisitionsAttempted());
    acq.endCycle();
    TEST_ASSERT_EQUAL_UINT16(0, acq.acquisitionsAttempted());
}

// T32 — DATA_RDY governs. Storing on a not-ready read would fill the buffer with
// repeats of the last sample at the polling rate rather than at the ODR, and the
// data would look plausible.
static void test_not_ready_samples_are_not_stored(void)
{
    FakeImu imu;
    RingBuffer ring(storage, kFrames, SAMPLE_SIZE_BYTES);
    AcquisitionService acq(imu, ring);
    acq.setConfig(defaults());
    imu.setReady(false);

    acq.beginAcquisition(0);
    for (uint32_t t = 1; t <= 20; t++)
        acq.step(t, TRIGGER_EVENT_NONE);

    TEST_ASSERT_EQUAL_UINT32(0, acq.bytesCollected());

    // And one ready read does store.
    imu.setReady(true);
    acq.step(21, TRIGGER_EVENT_NONE);
    TEST_ASSERT_EQUAL_UINT32(SAMPLE_SIZE_BYTES, acq.bytesCollected());
}

// T33 — the 18-byte wire frame: timestamp little-endian first, then the 14 sensor
// bytes exactly as the driver produced them. The server slices on this layout
// (backend/server/tcp_server.py) and the historical CSV corpus depends on it.
static void test_stored_frame_is_18_bytes_in_wire_order(void)
{
    FakeImu imu;
    RingBuffer ring(storage, kFrames, SAMPLE_SIZE_BYTES);
    AcquisitionService acq(imu, ring);
    acq.setConfig(defaults());

    imu.setReady(true);
    imu.setMarker(0x40);

    acq.beginAcquisition(0);
    acq.step(0x01020304, TRIGGER_EVENT_NONE);

    TEST_ASSERT_EQUAL_UINT32(SAMPLE_SIZE_BYTES, acq.bytesCollected());

    uint8_t expected[SAMPLE_SIZE_BYTES];
    expected[0] = 0x04; // timestamp, little-endian
    expected[1] = 0x03;
    expected[2] = 0x02;
    expected[3] = 0x01;
    for (uint32_t i = 0; i < IMU_SENSOR_BYTES; i++)
        expected[4 + i] = (uint8_t)(0x40 + i);

    ReadPlan plan = ring.plan();
    TEST_ASSERT_EQUAL_UINT32(SAMPLE_SIZE_BYTES, plan.totalBytes());
    TEST_ASSERT_EQUAL_MEMORY(expected, plan.first.ptr, SAMPLE_SIZE_BYTES);
}

// T33b — the timestamp is the one from BEFORE the sensor read, not after.
// Production samples millis() at ICM42688P.cpp:371 and only then does the SPI
// reads at :379; the refactor reversed it. At 50 Hz the skew is small, which is
// exactly why it would never be noticed.
static void test_frame_timestamp_is_the_one_passed_to_step(void)
{
    FakeImu imu;
    RingBuffer ring(storage, kFrames, SAMPLE_SIZE_BYTES);
    AcquisitionService acq(imu, ring);
    acq.setConfig(defaults());

    imu.setReady(true);
    acq.beginAcquisition(0);
    acq.step(0xAABBCCDD, TRIGGER_EVENT_NONE);

    ReadPlan plan = ring.plan();
    // Guard before dereferencing: an empty ring plans a null range, and a failed
    // assertion here longjmps out instead of segfaulting the whole suite.
    TEST_ASSERT_EQUAL_UINT32(SAMPLE_SIZE_BYTES, plan.totalBytes());
    TEST_ASSERT_EQUAL_UINT8(0xDD, plan.first.ptr[0]);
    TEST_ASSERT_EQUAL_UINT8(0xCC, plan.first.ptr[1]);
    TEST_ASSERT_EQUAL_UINT8(0xBB, plan.first.ptr[2]);
    TEST_ASSERT_EQUAL_UINT8(0xAA, plan.first.ptr[3]);
}

// A24 — starting an acquisition must NOT wake the IMU.
//
// Production wakes it once per boot (main.cpp:114). The refactor woke it inside
// every collect, re-programming PWR_MGMT0 and adding delay(45) to each one. "begin()
// wakes it exactly once" is a property of App and stays structural, but the harmful
// direction — waking per acquisition — is entirely inside this service, and this is
// what stops it coming back.
static void test_begin_acquisition_never_wakes_the_imu(void)
{
    FakeImu imu;
    RingBuffer ring(storage, kFrames, SAMPLE_SIZE_BYTES);
    AcquisitionService acq(imu, ring);
    acq.setConfig(defaults());

    acq.beginAcquisition(0);
    acq.step(1, TRIGGER_EVENT_STOP);
    acq.beginAcquisition(2);
    acq.step(3, TRIGGER_EVENT_STOP);
    acq.beginAcquisition(4);

    TEST_ASSERT_EQUAL_UINT32(0, imu.wakeCalls());
    TEST_ASSERT_EQUAL_UINT32(0, imu.sleepCalls());
}

static int run_all(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_begin_acquisition_never_wakes_the_imu);
    RUN_TEST(test_collect_stops_when_trigger_requests_stop);
    RUN_TEST(test_collect_stops_after_the_idle_timeout_with_no_trigger);
    RUN_TEST(test_collect_does_not_stop_before_the_idle_timeout);
    RUN_TEST(test_acquisition_count_accumulates_across_consecutive_collects);
    RUN_TEST(test_acquisition_count_is_not_advanced_by_steps);
    RUN_TEST(test_acquisition_count_increments_even_when_the_transmit_fails);
    RUN_TEST(test_acquisition_count_resets_when_the_cycle_ends_by_either_path);
    RUN_TEST(test_not_ready_samples_are_not_stored);
    RUN_TEST(test_stored_frame_is_18_bytes_in_wire_order);
    RUN_TEST(test_frame_timestamp_is_the_one_passed_to_step);
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
