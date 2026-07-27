#ifndef ESP32_PLATFORM_H
#define ESP32_PLATFORM_H

#include "Arduino.h"
#include "allocator.h"
#include "fault.h"

// The acquisition buffer lives in PSRAM — 700000 bytes will not fit in internal
// RAM.
class PsramAllocator : public IAllocator
{
public:
    uint8_t *allocate(uint32_t bytes) override
    {
        return (uint8_t *)ps_malloc(bytes);
    }
};

// Matches production exactly: print, then spin forever (main.cpp:61-62). It does
// not return and it deliberately does not reboot — a device that reboot-loops on a
// hardware fault is harder to diagnose in a plant than one sitting dead with a
// message on the serial line, and production chose the latter.
class HaltFault : public IFault
{
public:
    void fatal(const char *message) override
    {
        Serial.println(message);
        Serial.flush();
        while (true)
        {
        }
    }
};

#endif // ESP32_PLATFORM_H
