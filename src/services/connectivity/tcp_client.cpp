#include "tcp_client.h"
#include "Arduino.h"

bool TcpClient::open(const char *host, uint16_t port)
{
    Serial.println("Conectando ao servidor TCP..."); // main.cpp:206

    if (!_client.connect(host, port))
    {
        Serial.println("Falha na conexão ao servidor."); // main.cpp:213
        return false;
    }

    Serial.println("Conectado ao servidor."); // main.cpp:209
    return true;
}

uint32_t TcpClient::write(const uint8_t *data, uint32_t len)
{
    const uint32_t accepted = (uint32_t)_client.write(data, len);

    // A short write is not an error the caller can see — it just loops — but it
    // is the difference between "the link is stalling" and "the socket died",
    // and neither was visible from outside.
    if (accepted < len)
    {
        Serial.printf("Escrita curta: %u de %u bytes (socket %s)\n",
                      (unsigned)accepted, (unsigned)len,
                      _client.connected() ? "vivo" : "morto");
    }
    return accepted;
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
    // No print here. The server closes the connection as soon as it has sent the
    // config, so by the time we get here the socket is legitimately closed on
    // every successful cycle — the warning fired every time and taught the reader
    // to skip it. A socket that dies EARLY is still reported, by the short-write
    // line above, which is the case that actually means something.
    _client.stop();
}
