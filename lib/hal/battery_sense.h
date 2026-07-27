#ifndef BATTERY_SENSE_H
#define BATTERY_SENSE_H

#include <stdint.h>

// Battery voltage in millivolts, sampled at transmit time.
//
// It is a collaborator rather than a value passed into the cycle because production
// reads the ADC inside the transmit block (main.cpp:259), after the acquisition has
// finished. Hoisting it to the caller would move when the sample is taken.
class IBatterySense
{
public:
    virtual ~IBatterySense() {}
    virtual uint16_t readMv() = 0;
};

#endif // BATTERY_SENSE_H
