#include "power_manager.h"
#include "board.h"
#include "Arduino.h"
#include "esp_sleep.h"

uint16_t PowerManager::readBatteryMv()
{
    uint32_t raw = analogRead(BATTERY_ADC_PIN);
    return (uint16_t)((raw * BATTERY_MV_MAX) / BATTERY_ADC_MAX);
}

void PowerManager::sleepTimer(uint32_t minutes)
{
    uint64_t us = (uint64_t)minutes * 60ULL * 1000000ULL;
    esp_sleep_enable_timer_wakeup(us);
    esp_deep_sleep_start();
}

void PowerManager::sleepUntilTrigger()
{
    esp_sleep_enable_ext0_wakeup((gpio_num_t)MAGNET_TRIGGER_PIN, 0);
    esp_deep_sleep_start();
}
