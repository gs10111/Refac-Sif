#ifndef ACQUISITION_H
#define ACQUISITION_H

#include <stdint.h>

#include "packet.h"
#include "ring_buffer.h"
#include "belt_trigger.h"
#include "imu.h"

// One acquisition: poll the IMU, store 18-byte frames, stop when the belt comes
// round again or when the idle timeout says the belt has stopped.
//
// A step function, not a blocking loop. The tight polling belongs to the App,
// where the Arduino code lives; everything worth testing is in what one step
// decides. Same reasoning that kept BeltTrigger free of a clock seam: a call that
// already receives `now` has nothing left to abstract.
enum AcquisitionResult : uint8_t
{
    ACQ_RUNNING = 0,
    ACQ_STOPPED_BY_TRIGGER, // one belt revolution done — transmit this
    ACQ_STOPPED_BY_IDLE     // belt stopped — production discards this unsent
                            // (main.cpp:169 checks before the transmit block)
};

class AcquisitionService
{
public:
    AcquisitionService(IImu &imu, RingBuffer &ring);

    void setConfig(const ServerConfig &config);

    // What the IMU is ALREADY running, told by whoever configured it — App::begin,
    // with the rate it read from NVS. Explicit because the alternative was to
    // assume the first config seen describes the hardware, and it does not: the
    // service is built with the defaults, before NVS is read.
    void markSamplingApplied(uint16_t code);

    // Starts one acquisition and counts it. Wakes the IMU only when the rate has
    // changed since the last one — see the body. Does not clear the ring.
    void beginAcquisition(uint32_t nowMs);

    AcquisitionResult step(uint32_t nowMs, TriggerEvent trigger);

    // Iterations ATTEMPTED in this cycle, not successful transmissions — this
    // service never learns whether a transmit happened, which is what makes the
    // guarantee structural. See the note on BeltInputs::acquisitionsDone.
    uint16_t acquisitionsAttempted() const;

    // Production resets the counter on BOTH cycle-end paths (ICM42688P.cpp:438 for
    // the idle timeout, :445 for max_acq). Resetting on only one leaks into the
    // next cycle.
    void endCycle();

    uint32_t bytesCollected() const;

private:
    uint16_t _appliedSamplingCode = 0;  // what the IMU is actually running; 0 = unknown
    IImu &_imu;
    RingBuffer &_ring;
    ServerConfig _config;
    uint32_t _startMs;
    uint16_t _attempts;
};

#endif // ACQUISITION_H
