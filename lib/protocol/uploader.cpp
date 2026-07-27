#include "uploader.h"

// Production fragments the payload at 990 bytes (main.cpp:15, :237) and aborts the
// loop when a write is refused (:248-251).
#define UPLOAD_CHUNK_BYTES 990

// Production waits 2 s for the response (main.cpp:274).
#define CONFIG_RESPONSE_TIMEOUT_MS 2000

static bool write_all(ITransport &transport, const uint8_t *data, uint32_t len)
{
    uint32_t sent = 0;
    while (sent < len)
    {
        uint32_t chunk = len - sent;
        if (chunk > UPLOAD_CHUNK_BYTES)
            chunk = UPLOAD_CHUNK_BYTES;

        uint32_t accepted = transport.write(data + sent, chunk);
        if (accepted == 0)
            return false; // peer stopped taking data

        sent += accepted;
    }
    return true;
}

UploadOutcome upload_acquisition(ITransport &transport,
                                 const char *host,
                                 uint16_t port,
                                 RingBuffer &ring,
                                 uint16_t batteryMv,
                                 ServerConfig &configOut)
{
    UploadOutcome outcome = {false, false, false};

    if (!transport.open(host, port))
        return outcome; // never connected — the ring keeps its data for next time

    outcome.opened = true;

    ReadPlan plan = ring.plan();
    const uint32_t total = plan.totalBytes();

    uint8_t header[HEADER_SIZE_BYTES];
    header[0] = (uint8_t)(total & 0xFF);
    header[1] = (uint8_t)((total >> 8) & 0xFF);
    header[2] = (uint8_t)((total >> 16) & 0xFF);
    header[3] = (uint8_t)((total >> 24) & 0xFF);

    bool ok = write_all(transport, header, HEADER_SIZE_BYTES);

#ifdef MUTANT_SEND_FROM_INDEX_ZERO
    // ==== MUTATION: MUTANT_SEND_FROM_INDEX_ZERO ====
    // Built only by [env:mutant_send_from_index_zero].
    // Never by env:native or env:pico32.
    // Run it:  pio test -e mutant_send_from_index_zero
    //
    // BREAKS: only the first range goes out, starting at index 0, ignoring tail —
    //         R8 verbatim.
    // WHY:    it is correct until the ring wraps, which at 50 Hz takes thirteen
    //         minutes. That is why the regression survived review: the wrong code
    //         and the right code emit identical bytes for every short acquisition.
    // CAUGHT BY: test_transmit test_transmit_sends_oldest_first_when_the_ring_has_wrapped
    // SURVIVED BY: every other test in the suite. They all use an unwrapped ring,
    //         where tail is 0 and the two implementations agree byte for byte.
    if (ok && plan.first.len > 0)
        ok = write_all(transport, plan.first.ptr, plan.first.len);
#else
    // Oldest first: from tail to the end of the storage, then the frames that
    // overwrote the front (main.cpp:240 does the same with a modulo).
    if (ok && plan.first.len > 0)
        ok = write_all(transport, plan.first.ptr, plan.first.len);
    if (ok && plan.second.len > 0)
        ok = write_all(transport, plan.second.ptr, plan.second.len);
#endif

    uint8_t battery[BATTERY_SIZE_BYTES];
    battery[0] = (uint8_t)(batteryMv & 0xFF);
    battery[1] = (uint8_t)((batteryMv >> 8) & 0xFF);

    const bool batteryWritten = write_all(transport, battery, BATTERY_SIZE_BYTES);
    outcome.fullyWritten = ok && batteryWritten;

    // Unconditional, and that is production: the reset at main.cpp:264-265 sits
    // after the send loop, so a write that broke out at :248 still reaches it. A
    // partial send loses its data.
    ring.reset();

    uint8_t response[SERVER_CONFIG_WIRE_BYTES] = {0};
    const uint32_t received =
        transport.readExact(response, SERVER_CONFIG_WIRE_BYTES, CONFIG_RESPONSE_TIMEOUT_MS);

    // Through the decoder, not straight into the struct: a short frame must leave
    // the previous config exactly as it was.
    outcome.configReceived = parse_server_config(response, received, configOut);

    transport.close();
    return outcome;
}
