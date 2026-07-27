#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <stdint.h>

// A byte stream to the server.
//
// readExact carries its own timeout so the clock stays on the adapter's side of
// the seam: the fake just returns fewer bytes than were asked for. The backend
// converged on the same shape independently for its recv_exact.
class ITransport
{
public:
    virtual ~ITransport() {}

    virtual bool open(const char *host, uint16_t port) = 0;

    // Bytes actually accepted. Zero means the peer stopped taking data.
    virtual uint32_t write(const uint8_t *data, uint32_t len) = 0;

    // Blocks for up to timeoutMs. A short return means the peer never sent enough.
    virtual uint32_t readExact(uint8_t *out, uint32_t len, uint32_t timeoutMs) = 0;

    virtual void close() = 0;
};

#endif // TRANSPORT_H
