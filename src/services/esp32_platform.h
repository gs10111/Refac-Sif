#ifndef ESP32_PLATFORM_H
#define ESP32_PLATFORM_H

#include "Arduino.h"
#include "esp_sleep.h"
#include "board.h"
#include "allocator.h"
#include "fault.h"
#include "cpu.h"
#include "sleeper.h"

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


class EspCpu : public ICpu
{
public:
    void setFrequencyMhz(uint32_t mhz) override { setCpuFrequencyMhz(mhz); }
    void delayMs(uint32_t ms) override { delay(ms); }
};

class EspSleeper : public ISleeper
{
public:
    void enableTimerWakeup(uint64_t us) override
    {
        esp_sleep_enable_timer_wakeup(us);
    }

    void enableExt0Wakeup(uint8_t pin, uint8_t level) override
    {
        esp_sleep_enable_ext0_wakeup((gpio_num_t)pin, level);
    }

    void start() override
    {
        // Production flushes before every deep sleep (main.cpp:144 and :152). A
        // message still sitting in the TX buffer when the CPU stops is a message
        // nobody ever sees.
        Serial.flush();
        esp_deep_sleep_start();
    }
};

// Battery sense on the ADC pin. Same arithmetic as production's
// map(raw, 0, 4095, 0, 19803) at main.cpp:259-260, which is an integer map with a
// zero origin, so the multiply-then-divide is equivalent. 4095 * 19803 fits a
// uint32 with room to spare.
inline uint16_t esp32_read_battery_mv()
{
    uint32_t raw = analogRead(BATTERY_ADC_PIN);
    return (uint16_t)((raw * BATTERY_MV_MAX) / BATTERY_ADC_MAX);
}

#endif // ESP32_PLATFORM_H
