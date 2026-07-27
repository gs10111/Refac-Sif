// Round 5 — T34..T36 — allocating the acquisition buffer, and failing safely.
//
// R1, the blocker: the refactor asked ps_malloc for
// ACQUISITION_BUFFER_SAMPLES(350000) * SAMPLE_SIZE_BYTES(18) = 6,300,000 bytes,
// never checked the result, and stored the first sample through the null pointer.
// No ESP32 can map 6.3 MB, so every magnet trigger crashed the device. Production
// asks for 700000, checks, prints, and halts (main.cpp:56-65).
//
// Round 4 fixed the SIZE. It did not restore the CHECK: ps_malloc is still
// unchecked at App::begin, and the only reason that no longer crashes is that
// RingBuffer refuses to write through a null storage pointer. So today a failed
// allocation produces a device that boots, samples nothing, transmits nothing, and
// says nothing. Better than a boot loop and still wrong. This round makes it loud.
//
// TWO SEAMS, and the second one is subtle:
//   IAllocator  so a test can make the allocation fail on demand.
//   IFault      so "halt" is observable. In production fatal() never returns —
//               it prints and spins, like main.cpp:62. A test double MUST return,
//               or the test cannot continue. That means the code after a fatal()
//               call has to be safe even though production never reaches it, and
//               T36 is what pins that.

#include <unity.h>
#include <stdint.h>
#include <string.h>

#include "packet.h"
#include "board.h"
#include "ring_buffer.h"
#include "allocator.h"
#include "fault.h"
#include "sample_store.h"

void setUp(void) {}
void tearDown(void) {}

// --- fakes ----------------------------------------------------------------
static uint8_t backing[64 * SAMPLE_SIZE_BYTES];

class FakeAllocator : public IAllocator
{
public:
    FakeAllocator() : _fail(false), _requested(0), _calls(0) {}

    uint8_t *allocate(uint32_t bytes) override
    {
        _requested = bytes;
        _calls++;
        return _fail ? nullptr : backing;
    }

    void failNext() { _fail = true; }
    uint32_t requested() const { return _requested; }
    uint32_t calls() const { return _calls; }

private:
    bool _fail;
    uint32_t _requested;
    uint32_t _calls;
};

class RecordingFault : public IFault
{
public:
    RecordingFault() : _calls(0) { _last[0] = '\0'; }

    // Returns, unlike production. Everything after a fatal() call must be safe.
    void fatal(const char *message) override
    {
        _calls++;
        if (message != nullptr)
        {
            strncpy(_last, message, sizeof(_last) - 1);
            _last[sizeof(_last) - 1] = '\0';
        }
    }

    uint32_t calls() const { return _calls; }
    const char *last() const { return _last; }

private:
    uint32_t _calls;
    char _last[128];
};

// T34 — the allocation asks for exactly the number of bytes production asks for.
// The unit in the name is the whole of R1: 350000 counted int16 WORDS in the
// original, and reading it as SAMPLES turned a 700 kB request into 6.3 MB.
static void test_allocation_requests_exactly_the_production_buffer_size(void)
{
    FakeAllocator alloc;
    RecordingFault fault;

    RingBuffer ring = make_sample_ring(alloc, fault,
                                       ACQUISITION_BUFFER_BYTES, SAMPLE_SIZE_BYTES);

    TEST_ASSERT_EQUAL_UINT32(1, alloc.calls());
    TEST_ASSERT_EQUAL_UINT32(700000, alloc.requested());
    TEST_ASSERT_EQUAL_UINT32(0, fault.calls());

    // And the ring is addressed in whole frames, not in the raw byte count.
    TEST_ASSERT_EQUAL_UINT32(ACQUISITION_FRAME_CAPACITY, ring.frameCapacity());
}

// T35 — a failed allocation is loud. Production prints and halts (main.cpp:61-62);
// the refactor did neither and dereferenced the null pointer.
static void test_allocation_failure_calls_fatal(void)
{
    FakeAllocator alloc;
    RecordingFault fault;
    alloc.failNext();

    RingBuffer ring = make_sample_ring(alloc, fault,
                                       ACQUISITION_BUFFER_BYTES, SAMPLE_SIZE_BYTES);
    (void)ring;

    TEST_ASSERT_EQUAL_UINT32(1, fault.calls());
    TEST_ASSERT_TRUE(strlen(fault.last()) > 0);
}

// T36 — and nothing is written afterwards.
//
// In production fatal() never returns, so this path does not exist there. It
// exists here because a test double must return, and that makes it the one place
// where "what happens after a fatal we cannot actually have" is checkable. The
// ring must come back empty and refuse every append rather than carrying a
// capacity it has no storage for.
static void test_no_sample_is_ever_written_when_allocation_failed(void)
{
    FakeAllocator alloc;
    RecordingFault fault;
    alloc.failNext();

    RingBuffer ring = make_sample_ring(alloc, fault,
                                       ACQUISITION_BUFFER_BYTES, SAMPLE_SIZE_BYTES);

    uint8_t frame[SAMPLE_SIZE_BYTES];
    memset(frame, 0xA5, sizeof(frame));

    for (uint32_t i = 0; i < 10; i++)
        TEST_ASSERT_FALSE(ring.append(frame));

    TEST_ASSERT_EQUAL_UINT32(0, ring.bytesStored());
    TEST_ASSERT_EQUAL_UINT32(0, ring.plan().totalBytes());
}

// T36b — and it reports no capacity either.
//
// Separate from T36 on purpose: "nothing is written" and "the ring admits it has
// nowhere to write" are different claims, and they are defended in different
// places. Nothing is written because RingBuffer refuses a null storage pointer;
// the capacity is zero only because make_sample_ring chooses to say so. Folding
// both into one test would hide which of the two a change had broken.
static void test_failed_allocation_yields_a_ring_that_reports_no_capacity(void)
{
    FakeAllocator alloc;
    RecordingFault fault;
    alloc.failNext();

    RingBuffer ring = make_sample_ring(alloc, fault,
                                       ACQUISITION_BUFFER_BYTES, SAMPLE_SIZE_BYTES);

    TEST_ASSERT_EQUAL_UINT32(0, ring.frameCapacity());
}

static int run_all(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_allocation_requests_exactly_the_production_buffer_size);
    RUN_TEST(test_allocation_failure_calls_fatal);
    RUN_TEST(test_no_sample_is_ever_written_when_allocation_failed);
    RUN_TEST(test_failed_allocation_yields_a_ring_that_reports_no_capacity);
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
