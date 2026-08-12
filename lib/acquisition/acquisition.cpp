#include "acquisition.h"

AcquisitionService::AcquisitionService(IImu &imu, RingBuffer &ring)
    : _imu(imu),
      _ring(ring),
      _config(default_server_config()),
      _startMs(0),
      _attempts(0)
{
}

void AcquisitionService::setConfig(const ServerConfig &config)
{
    _config = config;
}

void AcquisitionService::markSamplingApplied(uint16_t code)
{
    _appliedSamplingCode = code;
}

void AcquisitionService::beginAcquisition(uint32_t nowMs)
{
    _startMs = nowMs;
    _attempts++;

    // A rate that arrived from the server mid-cycle takes effect HERE, on the
    // acquisition that follows it, instead of waiting for the next boot. D1 keeps
    // the device awake between acquisitions, so "next boot" could be four hours
    // away — an operator changing the rate on the page watched capture after
    // capture at the old one.
    //
    // Only on a change: the ODR nibble is written while the part comes out of OFF,
    // so applying it means waking the IMU, and waking costs the gyroscope its
    // settle time. Every capture answers with a rate; re-waking on each would
    // spend that time forever for nothing.
    if (_config.sampling_code != _appliedSamplingCode)
    {
        _imu.setSamplingCode((uint8_t)_config.sampling_code);
        _imu.wake();
        _appliedSamplingCode = _config.sampling_code;
    }

    // Deliberately does NOT wake the IMU or clear the ring.
    //
    // Production wakes the IMU once per boot (main.cpp:114), not per acquisition,
    // and clears head/tail at boot (main.cpp:158-159) and after a SUCCESSFUL send
    // (main.cpp:264-265) — never at the start of a collect. That second point
    // carries data: when a transmit fails, the next acquisition appends behind the
    // previous one and the next successful send carries both.
}

AcquisitionResult AcquisitionService::step(uint32_t nowMs, TriggerEvent trigger)
{
    // Production tests stopFlag at the top of the sampling loop (ICM42688P.cpp:364),
    // so the iteration that stops does not take a sample.
    if (trigger == TRIGGER_EVENT_STOP)
        return ACQ_STOPPED_BY_TRIGGER;

    uint8_t frame[SAMPLE_SIZE_BYTES];

    // Timestamp before the read, matching ICM42688P.cpp:371 ahead of the burst
    // at :379. `nowMs` is the caller's, so the late-timestamp regression cannot
    // be written here.
    frame[0] = (uint8_t)(nowMs & 0xFF);
    frame[1] = (uint8_t)((nowMs >> 8) & 0xFF);
    frame[2] = (uint8_t)((nowMs >> 16) & 0xFF);
    frame[3] = (uint8_t)((nowMs >> 24) & 0xFF);

    ImuStatus status = _imu.readSensorFrame(&frame[FRAME_TIMESTAMP_BYTES]);

#ifdef MUTANT_STORE_WHEN_NOT_READY
    // ==== MUTATION: MUTANT_STORE_WHEN_NOT_READY ====
    // Built only by [env:mutant_store_when_not_ready].
    // Never by env:native or env:pico32.
    // Run it:  pio test -e mutant_store_when_not_ready
    //
    // BREAKS: DATA_RDY governs whether a sample exists. This stores the frame
    //         regardless, so the buffer fills at the POLLING rate instead of the
    //         ODR, repeating whatever was last in the sensor registers.
    // WHY:    the resulting CSV is entirely plausible — right column count, right
    //         value ranges, monotonic timestamps — just sampled at the wrong rate.
    //         Nothing outside this check would notice.
    // CAUGHT BY: test_acquisition T32. T33 and T33b stay green under it: they set
    //         the fake ready, so the mutation is invisible to them.
    (void)status;
    _ring.append(frame);
#else
    if (status == IMU_OK)
        _ring.append(frame);
#endif

    // Production checks the idle timeout after the sampling block (:436) with a
    // strict `>`.
#ifdef MUTANT_IDLE_TIMEOUT_IN_SECONDS
    // ==== MUTATION: MUTANT_IDLE_TIMEOUT_IN_SECONDS ====
    // Built only by [env:mutant_idle_timeout_in_seconds].
    // Never by env:native or env:pico32.
    // Run it:  pio test -e mutant_idle_timeout_in_seconds
    //
    // BREAKS: idle_timeout_min is treated as SECONDS — 1000 instead of 60000.
    // WHY:    it is the realistic bug, not a contrived one. The device still
    //         stops, still stops at a timeout, and every endpoint assertion still
    //         holds; it just sleeps after 20 seconds instead of 20 minutes for the
    //         rest of its service life. This codebase already carries the same
    //         family at buttonTask's vTaskDelay(seconds * 1000), correct only
    //         because the tick happens to be 1 kHz.
    // CAUGHT BY: test_acquisition T29, at its middle probe of 20001 ms, which
    //         exists for exactly this reason. T28 does NOT catch it — a 20-second
    //         timeout also stops at 1200001 ms, so T28 only pins the endpoint.
    const uint32_t timeoutMs = (uint32_t)_config.idle_timeout_min * 1000UL;
#else
    const uint32_t timeoutMs = (uint32_t)_config.idle_timeout_min * 60UL * 1000UL;
#endif

    if ((nowMs - _startMs) > timeoutMs)
        return ACQ_STOPPED_BY_IDLE;

    return ACQ_RUNNING;
}

uint16_t AcquisitionService::acquisitionsAttempted() const
{
    return _attempts;
}

void AcquisitionService::endCycle()
{
    _attempts = 0;
}

uint32_t AcquisitionService::bytesCollected() const
{
    return _ring.bytesStored();
}
