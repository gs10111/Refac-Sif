#include "tcp_client.h"
#include "board.h"

bool TcpClient::connect(const char *host, uint16_t port)
{
    return _client.connect(host, port);
}

bool TcpClient::sendData(const uint8_t *buf, uint32_t bytes,
                         uint16_t batteryMv, ServerConfig &configOut)
{
    // 1. Send 4-byte header
    uint8_t header[HEADER_SIZE_BYTES];
    header[0] = (bytes      ) & 0xFF;
    header[1] = (bytes >>  8) & 0xFF;
    header[2] = (bytes >> 16) & 0xFF;
    header[3] = (bytes >> 24) & 0xFF;
    _client.write(header, HEADER_SIZE_BYTES);

    // 2. Send sample data
    _client.write(buf, bytes);

    // 3. Send battery voltage
    uint8_t batt[BATTERY_SIZE_BYTES];
    batt[0] = (batteryMv     ) & 0xFF;
    batt[1] = (batteryMv >> 8) & 0xFF;
    _client.write(batt, BATTERY_SIZE_BYTES);

    // 4. Wait for ServerConfig response (8 bytes)
    uint32_t start = millis();
    while (_client.available() < (int)sizeof(ServerConfig)) {
        if ((millis() - start) > 5000) return false;
        delay(10);
    }
    _client.readBytes((uint8_t *)&configOut, sizeof(ServerConfig));
    return true;
}

void TcpClient::disconnect()
{
    _client.stop();
}
