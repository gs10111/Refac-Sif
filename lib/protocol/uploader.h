#ifndef UPLOADER_H
#define UPLOADER_H

#include <stdint.h>

#include "packet.h"
#include "ring_buffer.h"
#include "transport.h"
#include "battery_sense.h"

struct UploadOutcome
{
    bool opened;          // the connection came up; the ring has been cleared
    bool fullyWritten;    // header, every sample byte and the battery all accepted
    bool configReceived;  // a full 12-byte response arrived and was applied
};

// One acquisition, one connection: open, send, clear, read the config, close.
//
// It opens the connection itself rather than being handed an open one, because
// whether the ring is cleared depends on how far we got — and if the caller owned
// the open, the branch deciding that would sit in Arduino code where nothing can
// assert it.
//
// The battery is a COLLABORATOR, not a value: production samples the ADC at
// main.cpp:259, after the entire payload has gone out — radio hot, cell under
// sustained transmit load. A value passed in would be read before the connection
// even opened, and a battery measured idle reads healthier than the same battery
// measured under load. Sag under load is how a tired cell announces itself, so the
// loaded reading is the diagnostic one, and every historical battery_mv in the
// corpus was measured that way.
//
// Production's rule, which this reproduces: the ring is preserved only if the
// connection never opened (main.cpp:212-219 returns out of loop()). Once it has
// opened, main.cpp:264-265 clears head and tail after the send loop —
// unconditionally, so a write that died mid-stream loses its data too.
UploadOutcome upload_acquisition(ITransport &transport,
                                 const char *host,
                                 uint16_t port,
                                 RingBuffer &ring,
                                 IBatterySense &battery,
                                 ServerConfig &configOut);

#endif // UPLOADER_H
