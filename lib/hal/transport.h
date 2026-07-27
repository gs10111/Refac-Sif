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
    //
    // CONTRACT, and upload_acquisition is only correct while it holds: A TRANSPORT
    // WHOSE write() RETURNS 0 MUST STAY FAILED. The uploader abandons the send on
    // the first refusal and never retries, because on TCP a refused write means the
    // connection is gone. A transport that returned 0 for a transient condition —
    // a full buffer, a would-block — would silently truncate every upload. This is
    // written here because the interface exists precisely so it can be swapped.
    virtual uint32_t write(const uint8_t *data, uint32_t len) = 0;

    // Blocks for up to timeoutMs. A short return means the peer never sent enough.
    virtual uint32_t readExact(uint8_t *out, uint32_t len, uint32_t timeoutMs) = 0;

    // CONTRACT: close() MUST BE IDEMPOTENT. upload_acquisition tears the socket
    // down on a refused write and closes again on the way out, so a failed upload
    // calls this twice. WiFiClient::stop() tolerates it; an implementation that did
    // not would fault on exactly the path that is hardest to reproduce.
    //
    // Closing promptly is also load-bearing on the SERVER side: the FIN lets its
    // recv fail at once instead of waiting out a six-second socket timeout holding
    // a thread-pool worker, one of ten, per failed upload.
    virtual void close() = 0;
};

#endif // TRANSPORT_H
