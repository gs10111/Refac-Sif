#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H
#include <stdint.h>
#include "WiFi.h"

class WiFiManager {
public:
    void configure(IPAddress ip, IPAddress gateway, IPAddress subnet);
    bool connect(const char *ssid, const char *password, uint32_t timeoutMs = 10000);
    void disconnect();
    bool isConnected();
};

#endif // WIFI_MANAGER_H
