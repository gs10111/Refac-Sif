// Round 7 — T43..T48 — the upload, and R8.
//
// R8: production sends from `tail`, wrapping (main.cpp:240,
// `(bytesSentTotal + IIM.tail) % byteBufferSize`). The refactor sent linearly from
// index 0. While the ring has not wrapped those are the same bytes; once it has,
// the refactor emits the newest frames first and the timestamps in the CSV go
// backwards mid-file.
//
// The whole upload lives in one function so the failure paths are reachable from a
// host build — including opening the connection, because whether the ring is
// cleared depends on how far we got.

#include <unity.h>
#include <stdint.h>
#include <string.h>

#include "packet.h"
#include "ring_buffer.h"
#include "transport.h"
#include "uploader.h"

void setUp(void) {}
void tearDown(void) {}

// --- fake transport -------------------------------------------------------
static const uint32_t kCaptureMax = 4096;

class FakeTransport : public ITransport
{
public:
    FakeTransport()
        : _openSucceeds(true), _writeLimit(kCaptureMax), _written(0),
          _responseLen(0), _closed(false), _openCalls(0) {}

    bool open(const char *, uint16_t) override
    {
        _openCalls++;
        return _openSucceeds;
    }

    uint32_t write(const uint8_t *data, uint32_t len) override
    {
        uint32_t room = (_writeLimit > _written) ? (_writeLimit - _written) : 0;
        uint32_t n = (len < room) ? len : room;
        for (uint32_t i = 0; i < n && _written < kCaptureMax; i++)
            _capture[_written++] = data[i];
        return n;
    }

    uint32_t readExact(uint8_t *out, uint32_t len, uint32_t) override
    {
        uint32_t n = (len < _responseLen) ? len : _responseLen;
        for (uint32_t i = 0; i < n; i++)
            out[i] = _response[i];
        return n; // short read means the peer never sent enough
    }

    void close() override { _closed = true; }

    void failOpen() { _openSucceeds = false; }
    void truncateWritesAt(uint32_t bytes) { _writeLimit = bytes; }
    void scriptResponse(const uint8_t *bytes, uint32_t len)
    {
        _responseLen = (len < sizeof(_response)) ? len : sizeof(_response);
        for (uint32_t i = 0; i < _responseLen; i++)
            _response[i] = bytes[i];
    }

    const uint8_t *captured() const { return _capture; }
    uint32_t capturedLen() const { return _written; }
    bool closed() const { return _closed; }
    uint32_t openCalls() const { return _openCalls; }

private:
    bool _openSucceeds;
    uint32_t _writeLimit;
    uint8_t _capture[kCaptureMax];
    uint32_t _written;
    uint8_t _response[32];
    uint32_t _responseLen;
    bool _closed;
    uint32_t _openCalls;
};

// --- fixture --------------------------------------------------------------
static const uint32_t kFrames = 4;
static uint8_t storage[kFrames * SAMPLE_SIZE_BYTES];

static void fill_ring(RingBuffer &ring, uint8_t first, uint8_t last)
{
    uint8_t frame[SAMPLE_SIZE_BYTES];
    for (uint8_t m = first; m <= last; m++)
    {
        memset(frame, m, SAMPLE_SIZE_BYTES);
        ring.append(frame);
    }
}

static const uint8_t kGoldenConfig[SERVER_CONFIG_WIRE_BYTES] = {
    0xF0, 0x00, 0x14, 0x00, 0x05, 0x00, 0x05, 0x00, 0x01, 0x00};

// T43 — the wire order: 4-byte length, then the frames, then 2 bytes of battery.
static void test_transmit_writes_header_then_samples_then_battery(void)
{
    RingBuffer ring(storage, kFrames, SAMPLE_SIZE_BYTES);
    fill_ring(ring, 1, 2);

    FakeTransport transport;
    transport.scriptResponse(kGoldenConfig, sizeof(kGoldenConfig));
    ServerConfig config = default_server_config();

    UploadOutcome out = upload_acquisition(transport, "host", 12345, ring, 0x0BB8, config);

    TEST_ASSERT_TRUE(out.opened);
    TEST_ASSERT_TRUE(out.fullyWritten);

    const uint32_t payload = 2 * SAMPLE_SIZE_BYTES;
    TEST_ASSERT_EQUAL_UINT32(HEADER_SIZE_BYTES + payload + BATTERY_SIZE_BYTES,
                             transport.capturedLen());

    const uint8_t *w = transport.captured();
    TEST_ASSERT_EQUAL_UINT8(1, w[HEADER_SIZE_BYTES]);                    // first frame
    TEST_ASSERT_EQUAL_UINT8(2, w[HEADER_SIZE_BYTES + SAMPLE_SIZE_BYTES]); // second
    TEST_ASSERT_EQUAL_UINT8(0xB8, w[HEADER_SIZE_BYTES + payload]);        // battery LE
    TEST_ASSERT_EQUAL_UINT8(0x0B, w[HEADER_SIZE_BYTES + payload + 1]);
}

// T44 — the header is the sample byte count, little-endian. The server blocks
// waiting for exactly this many bytes.
static void test_header_is_total_sample_bytes_little_endian(void)
{
    RingBuffer ring(storage, kFrames, SAMPLE_SIZE_BYTES);
    fill_ring(ring, 1, 3);

    FakeTransport transport;
    transport.scriptResponse(kGoldenConfig, sizeof(kGoldenConfig));
    ServerConfig config = default_server_config();

    upload_acquisition(transport, "host", 12345, ring, 0, config);

    const uint32_t payload = 3 * SAMPLE_SIZE_BYTES; // 54
    const uint8_t *w = transport.captured();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(payload & 0xFF), w[0]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)((payload >> 8) & 0xFF), w[1]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)((payload >> 16) & 0xFF), w[2]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)((payload >> 24) & 0xFF), w[3]);
}

// T45 — R8. Six frames into a four-frame ring leaves storage as [5][6][3][4] with
// tail at frame 2. The bytes must go out 3, 4, 5, 6 — oldest first, both ranges,
// in order. Sending from index 0 emits 5, 6, 3, 4 and the timestamps go backwards.
static void test_transmit_sends_oldest_first_when_the_ring_has_wrapped(void)
{
    RingBuffer ring(storage, kFrames, SAMPLE_SIZE_BYTES);
    fill_ring(ring, 1, 6);

    FakeTransport transport;
    transport.scriptResponse(kGoldenConfig, sizeof(kGoldenConfig));
    ServerConfig config = default_server_config();

    UploadOutcome out = upload_acquisition(transport, "host", 12345, ring, 0, config);
    TEST_ASSERT_TRUE(out.fullyWritten);

    const uint8_t *w = transport.captured() + HEADER_SIZE_BYTES;
    TEST_ASSERT_EQUAL_UINT8(3, w[0 * SAMPLE_SIZE_BYTES]);
    TEST_ASSERT_EQUAL_UINT8(4, w[1 * SAMPLE_SIZE_BYTES]);
    TEST_ASSERT_EQUAL_UINT8(5, w[2 * SAMPLE_SIZE_BYTES]);
    TEST_ASSERT_EQUAL_UINT8(6, w[3 * SAMPLE_SIZE_BYTES]);

    // And the header counts the whole buffer, not just the first range.
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(kFrames * SAMPLE_SIZE_BYTES), transport.captured()[0]);
}

// T46 — parse_server_config on the LIVE path, not beside it.
//
// The decoder has always required the full 10 bytes and left its output untouched
// on a short frame; the client read straight into the struct instead, so that
// guarantee protected nothing. A truncated response must leave the previous config
// exactly as it was.
static void test_config_is_applied_only_when_ten_bytes_arrive(void)
{
    RingBuffer ring(storage, kFrames, SAMPLE_SIZE_BYTES);
    fill_ring(ring, 1, 1);

    FakeTransport transport;
    transport.scriptResponse(kGoldenConfig, SERVER_CONFIG_WIRE_BYTES - 1); // 9 bytes
    ServerConfig config = default_server_config();

    UploadOutcome out = upload_acquisition(transport, "host", 12345, ring, 0, config);

    TEST_ASSERT_FALSE(out.configReceived);
    TEST_ASSERT_EQUAL_UINT16(DEFAULT_SLEEP_TIME_MIN, config.sleep_time_min);
    TEST_ASSERT_EQUAL_UINT16(DEFAULT_IDLE_TIMEOUT_MIN, config.idle_timeout_min);
    TEST_ASSERT_EQUAL_UINT16(DEFAULT_MAX_ACQUISITIONS, config.max_acquisitions);
    TEST_ASSERT_EQUAL_UINT16(DEFAULT_TRIGGER_COOLDOWN_SEC, config.trigger_cooldown_sec);
    TEST_ASSERT_EQUAL_UINT16(DEFAULT_UPDATE, config.update);
}

// And the whole 10 bytes are taken when they do arrive, update included.
static void test_config_is_applied_when_all_ten_bytes_arrive(void)
{
    RingBuffer ring(storage, kFrames, SAMPLE_SIZE_BYTES);
    fill_ring(ring, 1, 1);

    FakeTransport transport;
    transport.scriptResponse(kGoldenConfig, SERVER_CONFIG_WIRE_BYTES);
    ServerConfig config = default_server_config();

    UploadOutcome out = upload_acquisition(transport, "host", 12345, ring, 0, config);

    TEST_ASSERT_TRUE(out.configReceived);
    TEST_ASSERT_EQUAL_UINT16(240, config.sleep_time_min);
    TEST_ASSERT_EQUAL_UINT16(1, config.update);
}

// T47 — a socket that stops accepting bytes is reported, not ignored. The refactor
// discarded write()'s return value entirely.
static void test_transmit_reports_failure_on_short_write(void)
{
    RingBuffer ring(storage, kFrames, SAMPLE_SIZE_BYTES);
    fill_ring(ring, 1, 4);

    FakeTransport transport;
    transport.truncateWritesAt(HEADER_SIZE_BYTES + SAMPLE_SIZE_BYTES); // dies mid-payload
    transport.scriptResponse(kGoldenConfig, sizeof(kGoldenConfig));
    ServerConfig config = default_server_config();

    UploadOutcome out = upload_acquisition(transport, "host", 12345, ring, 0, config);

    TEST_ASSERT_TRUE(out.opened);
    TEST_ASSERT_FALSE(out.fullyWritten);
}

// T47b — the connection never opened: nothing was written and the data is still
// there for the next attempt. Production skips the whole transmit block when WiFi
// or the TCP connect fails (main.cpp:212-219 returns out of loop()), so the ring is
// never touched.
static void test_ring_is_preserved_when_the_connection_never_opened(void)
{
    RingBuffer ring(storage, kFrames, SAMPLE_SIZE_BYTES);
    fill_ring(ring, 1, 3);

    FakeTransport transport;
    transport.failOpen();
    ServerConfig config = default_server_config();

    UploadOutcome out = upload_acquisition(transport, "host", 12345, ring, 0, config);

    TEST_ASSERT_FALSE(out.opened);
    TEST_ASSERT_EQUAL_UINT32(0, transport.capturedLen());
    TEST_ASSERT_EQUAL_UINT32(3 * SAMPLE_SIZE_BYTES, ring.bytesStored());
}

// T47c — but once the connection HAS opened, the ring is cleared even if the write
// died mid-stream.
//
// This is production and it is counter-intuitive: main.cpp:248-251 breaks out of
// the send loop on a failed write, and :264-265 then clears head and tail anyway,
// because the reset sits after the loop rather than inside the success path. My
// round 4 note claimed the opposite — that only a successful send clears — and that
// was wrong about production. Pinned here so the behaviour is chosen rather than
// drifted into.
static void test_ring_is_cleared_once_the_connection_opened_even_if_the_write_failed(void)
{
    RingBuffer ring(storage, kFrames, SAMPLE_SIZE_BYTES);
    fill_ring(ring, 1, 3);

    FakeTransport transport;
    transport.truncateWritesAt(HEADER_SIZE_BYTES); // header only, payload rejected
    transport.scriptResponse(kGoldenConfig, sizeof(kGoldenConfig));
    ServerConfig config = default_server_config();

    UploadOutcome out = upload_acquisition(transport, "host", 12345, ring, 0, config);

    TEST_ASSERT_TRUE(out.opened);
    TEST_ASSERT_FALSE(out.fullyWritten);
    TEST_ASSERT_EQUAL_UINT32(0, ring.bytesStored());
}

// T48 — no response at all. The data still went out and the ring is still cleared;
// only the config is left alone. Production waits 2 s, prints "Nenhuma resposta
// recebida do servidor" and carries on (main.cpp:273-295), having already reset at
// :264-265.
static void test_missing_response_leaves_the_config_but_not_the_data(void)
{
    RingBuffer ring(storage, kFrames, SAMPLE_SIZE_BYTES);
    fill_ring(ring, 1, 2);

    FakeTransport transport;
    transport.scriptResponse(kGoldenConfig, 0); // silence
    ServerConfig config = default_server_config();

    UploadOutcome out = upload_acquisition(transport, "host", 12345, ring, 0, config);

    TEST_ASSERT_TRUE(out.fullyWritten);
    TEST_ASSERT_FALSE(out.configReceived);
    TEST_ASSERT_EQUAL_UINT32(0, ring.bytesStored());
    TEST_ASSERT_EQUAL_UINT16(DEFAULT_SLEEP_TIME_MIN, config.sleep_time_min);
    TEST_ASSERT_TRUE(transport.closed());
}

static int run_all(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_transmit_writes_header_then_samples_then_battery);
    RUN_TEST(test_header_is_total_sample_bytes_little_endian);
    RUN_TEST(test_transmit_sends_oldest_first_when_the_ring_has_wrapped);
    RUN_TEST(test_config_is_applied_only_when_ten_bytes_arrive);
    RUN_TEST(test_config_is_applied_when_all_ten_bytes_arrive);
    RUN_TEST(test_transmit_reports_failure_on_short_write);
    RUN_TEST(test_ring_is_preserved_when_the_connection_never_opened);
    RUN_TEST(test_ring_is_cleared_once_the_connection_opened_even_if_the_write_failed);
    RUN_TEST(test_missing_response_leaves_the_config_but_not_the_data);
    return UNITY_END();
}

#ifdef ARDUINO
#include <Arduino.h>
void setup()
{
    delay(2000);
    run_all();
}
void loop() {}
#else
int main(void)
{
    return run_all();
}
#endif
