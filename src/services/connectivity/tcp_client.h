#ifndef TCP_CLIENT_H
#define TCP_CLIENT_H
#include "WiFiClient.h"
#include "packet.h"
#include <stdint.h>

class TcpClient {
public:
    bool connect(const char *host, uint16_t port);
    bool sendData(const uint8_t *buf, uint32_t bytes, uint16_t batteryMv, ServerConfig &configOut);
    void disconnect();
private:
    WiFiClient _client;
};

#endif // TCP_CLIENT_H
