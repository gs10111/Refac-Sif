#ifndef SLEEPER_H
#define SLEEPER_H

#include <stdint.h>

// Deep sleep, split into arming and starting so the order is assertable.
//
// On the device start() never returns. A test double must return, so anything a
// caller does after start() has to be harmless.
class ISleeper
{
public:
    virtual ~ISleeper() {}

    // Microseconds, and it must be 64-bit: 240 minutes is 14,400,000,000 us, which
    // truncates to about 30 minutes in 32 bits while the source line still reads
    // correctly. Production casts at main.cpp:173.
    virtual void enableTimerWakeup(uint64_t us) = 0;

    // level 0 wakes on LOW, as the reed switch pulls the pin down (main.cpp:151).
    virtual void enableExt0Wakeup(uint8_t pin, uint8_t level) = 0;

    virtual void start() = 0;
};

#endif // SLEEPER_H
