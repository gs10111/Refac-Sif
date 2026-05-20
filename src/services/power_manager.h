#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H
#include <stdint.h>

class PowerManager {
public:
    uint16_t readBatteryMv();
    void     sleepTimer(uint32_t minutes);
    void     sleepUntilTrigger();
};

#endif // POWER_MANAGER_H
