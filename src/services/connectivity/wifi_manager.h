#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H
#include <stdint.h>
#include "WiFi.h"
#include "radio.h"

class WiFiManager : public IRadio {
public:
    void configure(IPAddress ip, IPAddress gateway, IPAddress subnet);
    // 5 s, as production waits at main.cpp:191. The refactor used 10.
    bool connect(const char *ssid, const char *password, uint32_t timeoutMs = 5000) override;
    void off() override;
    bool isConnected() override;
};

#endif // WIFI_MANAGER_H
