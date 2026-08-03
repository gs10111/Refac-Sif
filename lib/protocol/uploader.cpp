#include "uploader.h"

#include "log.h"

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
        {
            SIF_LOG("Erro ao enviar dados. Tentando reconectar..."); // main.cpp:249
            return false; // peer stopped taking data
        }

        sent += accepted;
    }
    return true;
}

UploadOutcome upload_acquisition(ITransport &transport,
                                 const char *host,
                                 uint16_t port,
                                 RingBuffer &ring,
                                 IBatterySense &battery,
                                 ServerConfig &configOut)
{
    UploadOutcome outcome = {false, false, false};
    SIF_TIMER(uploadStart);

    if (!transport.open(host, port))
        return outcome; // never connected — the ring keeps its data for next time

    outcome.opened = true;

#ifdef MUTANT_BATTERY_READ_BEFORE_UPLOAD
    // ==== MUTATION: MUTANT_BATTERY_READ_BEFORE_UPLOAD ====
    // Built only by [env:mutant_battery_read_before_upload].
    // Never by env:native or env:pico32.
    // Run it:  pio test -e mutant_battery_read_before_upload
    //
    // BREAKS: the ADC is sampled before a single byte goes out, with the radio
    //         connected but idle, instead of after the whole payload as production
    //         does at main.cpp:259.
    // WHY:    the conversion is untouched and the column keeps its name, so nothing
    //         about the CSV looks wrong. An unloaded cell simply reads healthier
    //         than a loaded one, and sag under load is the whole diagnostic. Every
    //         historical battery_mv was measured under load; these would not be, and
    //         any threshold spanning the change compares two different measurements.
    // CAUGHT BY: test_transmit test_battery_is_sampled_after_the_payload_and_before_
    //         its_own_write, which pins the byte count at the moment of the read.
    // SURVIVED BY: every other test — they assert the VALUE that lands on the wire,
    //         which is identical either way.
    const uint16_t batteryMv = battery.readMv();
#endif

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

    if (!ok)
    {
        // Production tears the socket down BEFORE breaking out of the send loop
        // (main.cpp:250, then :251). That single call is why it cannot apply a
        // config after a failed send: the battery write at :261 and the response
        // wait at :273 both reach a dead client and fail silently.
        //
        // What follows, precisely, because the obvious description of it is wrong:
        //
        //   the battery IS still sampled below, as production samples it at :259;
        //   the battery is NOT written, because `ok &&` short-circuits at :124 —
        //     production DOES reach client.write(battery) at :261, so this is a
        //     deliberate guard on top of the teardown and a real deviation;
        //   the ring IS still cleared, as production clears it at :264-265.
        //
        // The deviation puts nothing different on the wire: production's write lands
        // on the socket it stopped a moment earlier and fails silently, so both
        // versions send zero battery bytes after a refused payload. What the guard
        // buys is that the config-after-partial path is now unreachable TWICE — once
        // by the teardown, once by the short-circuit — instead of resting on the
        // transport behaving the way TCP does.
        transport.close();
    }


    uint8_t batteryBytes[BATTERY_SIZE_BYTES];
#ifndef MUTANT_BATTERY_READ_BEFORE_UPLOAD
    // main.cpp:259 — after the send loop has closed, radio hot, cell under load.
    const uint16_t batteryMv = battery.readMv();
#endif
    batteryBytes[0] = (uint8_t)(batteryMv & 0xFF);
    batteryBytes[1] = (uint8_t)((batteryMv >> 8) & 0xFF);

    outcome.fullyWritten = ok && write_all(transport, batteryBytes, BATTERY_SIZE_BYTES);

    // Unconditional, and that is production: the reset at main.cpp:264-265 sits
    // after the send loop, so a write that broke out at :248 still reaches it. A
    // partial send loses its data.
    ring.reset();

    if (!outcome.fullyWritten)
    {
        transport.close();
        return outcome;
    }

    uint8_t response[SERVER_CONFIG_WIRE_BYTES] = {0};
    const uint32_t received =
        transport.readExact(response, SERVER_CONFIG_WIRE_BYTES, CONFIG_RESPONSE_TIMEOUT_MS);

    // main.cpp:268-270. Production times this from the WiFi begin; ours starts at
    // the connection open, because the connect happens in WiFiManager. Same shape,
    // narrower span, and the number is a diagnostic rather than a measurement.
    SIF_LOG("Tempo de Conexão + Transmissão");
    SIF_LOG(SIF_ELAPSED(uploadStart));
#ifdef ARDUINO
    {
        const uint32_t ms = SIF_ELAPSED(uploadStart);
        const float kbps = ms ? ((total + BATTERY_SIZE_BYTES) * 8.0f) / (float)ms : 0.0f;
        SIF_LOGF("Transmissão concluída com throughput de %.2f kbps\n", kbps);
    }
#endif

    // Through the decoder, not straight into the struct: a short frame must leave
    // the previous config exactly as it was.
    outcome.configReceived = parse_server_config(response, received, configOut);

    if (outcome.configReceived)
    {
        SIF_LOGF("Recebido do servidor: %u, %u, %u, %u, %u\n", // main.cpp:281
                 configOut.sleep_time_min, configOut.idle_timeout_min,
                 configOut.max_acquisitions, configOut.trigger_cooldown_sec,
                 configOut.update);
    }
    else if (received > 0)
    {
        SIF_LOG("Resposta do servidor incompleta."); // main.cpp:291
    }
    else
    {
        SIF_LOG("Nenhuma resposta recebida do servidor."); // main.cpp:294
    }

    transport.close();
    return outcome;
}
