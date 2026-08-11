#ifndef IMU_H
#define IMU_H

#include <stdint.h>

#include "packet.h"

// The 14 SENSOR bytes of a wire frame: accel X, gyro X, accel Y, gyro Y, accel Z,
// gyro Z, temperature — each a little-endian int16, in the order the driver reads
// them off the bus.
//
// The 4-byte timestamp that precedes them is deliberately NOT the driver's to
// write. Production samples millis() BEFORE the SPI burst (ICM42688P.cpp:371, then
// the reads at :379); the refactor sampled it after all fourteen reads
// (lib/ICM42688P/ICM42688P.cpp:92), timestamping every frame roughly one SPI burst
// late — small, systematic, and on all 38888 frames. Handing the timestamp in from
// the caller makes the production ordering the only one that can be expressed.
#define IMU_SENSOR_BYTES 14

static_assert(FRAME_TIMESTAMP_BYTES + IMU_SENSOR_BYTES == SAMPLE_SIZE_BYTES,
              "timestamp plus sensor bytes must be the wire frame size");

enum ImuStatus : uint8_t
{
    IMU_NOT_READY = 0, // DATA_RDY was clear — there is no new sample
    IMU_OK
};

class IImu
{
public:
    virtual ~IImu() {}

    virtual void wake() = 0;
    virtual void sleep() = 0;

    // The ODR nibble to use from the next wake() on. Taking effect at wake() and
    // not immediately is the part's own rule: ACCEL_CONFIG0 and GYRO_CONFIG0 are
    // written while bringing the sensors out of OFF, which is what wake() does.
    virtual void setSamplingCode(uint8_t odrNibble) = 0;

    // Fills IMU_SENSOR_BYTES bytes. Returns IMU_NOT_READY without touching `out`
    // when DATA_RDY is clear.
    virtual ImuStatus readSensorFrame(uint8_t *out) = 0;
};

#endif // IMU_H
