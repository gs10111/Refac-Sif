#ifndef UPLOADER_H
#define UPLOADER_H

#include <stdint.h>

#include "packet.h"
#include "ring_buffer.h"
#include "transport.h"

struct UploadOutcome
{
    bool opened;          // the connection came up; the ring has been cleared
    bool fullyWritten;    // header, every sample byte and the battery all accepted
    bool configReceived;  // a full 10-byte response arrived and was applied
};

// One acquisition, one connection: open, send, clear, read the config, close.
//
// It opens the connection itself rather than being handed an open one, because
// whether the ring is cleared depends on how far we got — and if the caller owned
// the open, the branch deciding that would sit in Arduino code where nothing can
// assert it.
//
// Production's rule, which this reproduces: the ring is preserved only if the
// connection never opened (main.cpp:212-219 returns out of loop()). Once it has
// opened, main.cpp:264-265 clears head and tail after the send loop —
// unconditionally, so a write that died mid-stream loses its data too.
UploadOutcome upload_acquisition(ITransport &transport,
                                 const char *host,
                                 uint16_t port,
                                 RingBuffer &ring,
                                 uint16_t batteryMv,
                                 ServerConfig &configOut);

#endif // UPLOADER_H
