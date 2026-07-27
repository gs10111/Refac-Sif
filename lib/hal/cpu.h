#ifndef CPU_H
#define CPU_H

#include <stdint.h>

// CPU clock control, plus the settle delay that always pairs with a frequency
// change (main.cpp:176-177 raises to 240 MHz and waits 100 ms before bringing WiFi
// up). delayMs lives here rather than behind a fifth interface because that pairing
// is the only place it is used.
class ICpu
{
public:
    virtual ~ICpu() {}

    virtual void setFrequencyMhz(uint32_t mhz) = 0;
    virtual void delayMs(uint32_t ms) = 0;
};

#endif // CPU_H
