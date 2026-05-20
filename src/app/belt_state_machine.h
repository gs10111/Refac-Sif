#ifndef BELT_STATE_MACHINE_H
#define BELT_STATE_MACHINE_H

#include <stdint.h>

// Acquisition cycle states — persisted in RTC memory across deep sleep
// RTC_DATA_ATTR AcquisitionStage acquisitionStage in main.cpp
enum AcquisitionStage : uint8_t {
    STAGE_SLEEP_TIMER = 0,  // sleep fixed duration, wake by timer
    STAGE_WAIT_TRIGGER,     // sleep until magnet trigger fires on MAGNET_TRIGGER_PIN
    STAGE_ACQUIRE_TRANSMIT  // collect samples and transmit to server
};

#endif // BELT_STATE_MACHINE_H
