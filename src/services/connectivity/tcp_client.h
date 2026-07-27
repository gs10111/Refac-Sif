#ifndef TCP_CLIENT_H
#define TCP_CLIENT_H

#include <stdint.h>
#include "WiFiClient.h"
#include "transport.h"

// ITransport over WiFiClient. All the upload logic lives in upload_acquisition;
// this is the part that cannot be tested on a host.
class TcpClient : public ITransport
{
public:
    bool open(const char *host, uint16_t port) override;
    uint32_t write(const uint8_t *data, uint32_t len) override;
    uint32_t readExact(uint8_t *out, uint32_t len, uint32_t timeoutMs) override;
    void close() override;

private:
    WiFiClient _client;
};

#endif // TCP_CLIENT_H
