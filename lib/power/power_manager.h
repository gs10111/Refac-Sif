#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include <stdint.h>

#include "imu.h"
#include "radio.h"
#include "cpu.h"
#include "sleeper.h"

// The power path, expressed as ordered collaborator calls.
//
// It takes IImu, IRadio, ICpu and ISleeper not so it can be tested in isolation,
// but because "before" is the thing under test. R5 and R7 are both ordering
// defects — every call exists, they happen in the wrong order or on the wrong
// branch — and ordering is unassertable until the collaborators record. Both were
// on the hardware-only list in the first analysis; that was a design claim wearing
// a testability claim's clothes.
class PowerManager
{
public:
    PowerManager(IImu &imu, IRadio &radio, ICpu &cpu, ISleeper &sleeper);

    // Radio off, IMU asleep, then arm and go — the order production uses at
    // main.cpp:138 followed by :141.
    void deepSleepTimer(uint16_t minutes);
    void deepSleepUntilTrigger(uint8_t triggerPin);

    // Radio off first, then drop the clock: the WiFi stack was never meant to run
    // at 10 MHz.
    void enterAcquisitionMode();

    // Raise the clock, then settle before anything touches the radio.
    void enterTransmitMode();

private:
    IImu &_imu;
    IRadio &_radio;
    ICpu &_cpu;
    ISleeper &_sleeper;
};

#endif // POWER_MANAGER_H
