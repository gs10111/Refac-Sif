#include "tcp_client.h"
#include "Arduino.h"

bool TcpClient::open(const char *host, uint16_t port)
{
    return _client.connect(host, port);
}

uint32_t TcpClient::write(const uint8_t *data, uint32_t len)
{
    return (uint32_t)_client.write(data, len);
}

uint32_t TcpClient::readExact(uint8_t *out, uint32_t len, uint32_t timeoutMs)
{
    uint32_t got = 0;
    uint32_t start = millis();

    while (got < len)
    {
        if (_client.available() > 0)
        {
            int c = _client.read();
            if (c < 0)
                break;
            out[got++] = (uint8_t)c;
            continue;
        }

        if ((millis() - start) > timeoutMs)
            break;

        delay(10);
    }

    return got;
}

void TcpClient::close()
{
    _client.stop();
}
