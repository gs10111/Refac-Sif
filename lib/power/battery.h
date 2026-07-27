#ifndef BATTERY_H
#define BATTERY_H

#include <stdint.h>

#include "board.h"

// Production uses map(raw, 0, 4095, 0, 19803) at main.cpp:259-260. An integer map
// from a zero origin is exactly multiply-then-divide, so this is the same
// arithmetic; 4095 * 19803 fits a uint32 with room to spare.
//
// No clamp above full scale: analogRead is 12-bit and cannot produce one, and
// handling an impossible input would add behaviour production does not have.
inline uint16_t battery_mv_from_raw(uint16_t raw)
{
    return (uint16_t)(((uint32_t)raw * BATTERY_MV_MAX) / BATTERY_ADC_MAX);
}

#endif // BATTERY_H
