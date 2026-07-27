// Round 1 — T06..T11 — the acquisition ring buffer.
//
// Two regressions live here:
//   R1  the allocation size was read as SAMPLES when the original counted WORDS
//       (350000 * 18 = 6.3 MB instead of 350000 * 2 = 700000 B).
//   R8  transmission dropped the `tail` offset, so a wrapped buffer was sent
//       starting at index 0 and the timestamps went backwards mid-file.
//
// DEC-1 fixes both without moving off the number bigboss named: allocate exactly
// 700000 B, but express capacity in FRAMES — 700000 / 18 = 38888 frames = 699984
// usable bytes. The trailing 16 bytes are never written and never transmitted, so
// the payload is always a whole number of 18-byte frames and every wrap is
// frame-aligned.
//
// plan() semantics pinned here (DEC-9):
//   - append() writes at head, then head advances one frame (mod capacity).
//   - appending while full also advances tail one frame — the oldest frame is
//     overwritten, matching the original driver.
//   - tail == 0  -> ONE range from index 0. Byte-identical to production, which
//     always transmits from index 0 (its wrap check was dead code: an unsigned
//     `head - tail < 0` can never be true).
//   - tail != 0  -> two ranges, chronological, starting at tail. This is the path
//     the original never reached correctly.

#include <unity.h>
#include <stdint.h>
#include <string.h>

#include "board.h"
#include "packet.h"
#include "ring_buffer.h"

void setUp(void) {}
void tearDown(void) {}

static const uint32_t kFrameSize = SAMPLE_SIZE_BYTES;
static const uint32_t kFrames = 4;

static uint8_t storage[kFrames * SAMPLE_SIZE_BYTES];

// Frames are filled with a repeated marker byte so a range can be identified by
// its first byte alone.
static void make_frame(uint8_t *frame, uint8_t marker)
{
    memset(frame, marker, kFrameSize);
}

static void append_markers(RingBuffer &rb, uint8_t first, uint8_t last)
{
    uint8_t frame[kFrameSize];
    for (uint8_t m = first; m <= last; m++)
    {
        make_frame(frame, m);
        rb.append(frame);
    }
}

// T06 — 700000 is not a multiple of 18. The capacity the ring actually uses must
// be, or every wrap shifts the framing by 16 bytes and the server's fixed 18-byte
// slicing decodes garbage from that point on.
static void test_ring_capacity_is_whole_number_of_frames(void)
{
    TEST_ASSERT_EQUAL_UINT32(700000, (uint32_t)ACQUISITION_BUFFER_BYTES);

    const uint32_t frames =
        RingBuffer::frameCapacityFor((uint32_t)ACQUISITION_BUFFER_BYTES, kFrameSize);

    TEST_ASSERT_EQUAL_UINT32(38888, frames);
    TEST_ASSERT_EQUAL_UINT32(699984, frames * kFrameSize);
    TEST_ASSERT_EQUAL_UINT32(0, (frames * kFrameSize) % kFrameSize);
    TEST_ASSERT_EQUAL_UINT32(16, (uint32_t)ACQUISITION_BUFFER_BYTES - frames * kFrameSize);
}

// T07
static void test_append_advances_head_by_one_frame(void)
{
    RingBuffer rb(storage, kFrames, kFrameSize);
    TEST_ASSERT_EQUAL_UINT32(0, rb.bytesStored());

    append_markers(rb, 1, 1);
    TEST_ASSERT_EQUAL_UINT32(kFrameSize, rb.bytesStored());

    append_markers(rb, 2, 2);
    TEST_ASSERT_EQUAL_UINT32(2 * kFrameSize, rb.bytesStored());
}

// T08 — the non-wrapped path must stay byte-identical to production.
static void test_plan_is_one_range_from_zero_when_not_wrapped(void)
{
    RingBuffer rb(storage, kFrames, kFrameSize);
    append_markers(rb, 1, 2);

    ReadPlan plan = rb.plan();

    TEST_ASSERT_EQUAL_PTR(storage, plan.first.ptr);
    TEST_ASSERT_EQUAL_UINT32(2 * kFrameSize, plan.first.len);
    TEST_ASSERT_EQUAL_UINT32(0, plan.second.len);
    TEST_ASSERT_EQUAL_UINT32(2 * kFrameSize, plan.totalBytes());
    TEST_ASSERT_EQUAL_UINT8(1, plan.first.ptr[0]);
}

// T09 — six frames into a four-frame ring. Storage ends up [5][6][3][4] with
// tail at frame 2, so chronological order is 3, 4, 5, 6 — two ranges, oldest
// first. Sending from index 0 instead (the regression) would emit 5, 6, 3, 4.
static void test_plan_is_two_ranges_starting_at_tail_when_wrapped(void)
{
    RingBuffer rb(storage, kFrames, kFrameSize);
    append_markers(rb, 1, 6);

    ReadPlan plan = rb.plan();

    TEST_ASSERT_EQUAL_PTR(storage + 2 * kFrameSize, plan.first.ptr);
    TEST_ASSERT_EQUAL_UINT32(2 * kFrameSize, plan.first.len);
    TEST_ASSERT_EQUAL_UINT8(3, plan.first.ptr[0]);

    TEST_ASSERT_EQUAL_PTR(storage, plan.second.ptr);
    TEST_ASSERT_EQUAL_UINT32(2 * kFrameSize, plan.second.len);
    TEST_ASSERT_EQUAL_UINT8(5, plan.second.ptr[0]);

    TEST_ASSERT_EQUAL_UINT32(kFrames * kFrameSize, plan.totalBytes());
}

// T10 — five frames into a four-frame ring: frame 1 is gone, the oldest readable
// frame is 2.
static void test_append_overwrites_oldest_frame_when_full(void)
{
    RingBuffer rb(storage, kFrames, kFrameSize);
    append_markers(rb, 1, 5);

    ReadPlan plan = rb.plan();

    TEST_ASSERT_EQUAL_UINT32(kFrames * kFrameSize, rb.bytesStored());
    TEST_ASSERT_EQUAL_PTR(storage + kFrameSize, plan.first.ptr);
    TEST_ASSERT_EQUAL_UINT8(2, plan.first.ptr[0]);
    TEST_ASSERT_EQUAL_UINT32(3 * kFrameSize, plan.first.len);

    TEST_ASSERT_EQUAL_PTR(storage, plan.second.ptr);
    TEST_ASSERT_EQUAL_UINT32(kFrameSize, plan.second.len);
    TEST_ASSERT_EQUAL_UINT8(5, plan.second.ptr[0]);
}

// T11 — the 4-byte length header sent before the payload is bytesStored(). If it
// could exceed capacity the server would block waiting for bytes that never come.
static void test_bytes_stored_never_exceeds_capacity(void)
{
    RingBuffer rb(storage, kFrames, kFrameSize);

    uint8_t frame[kFrameSize];
    for (uint32_t i = 0; i < 100; i++)
    {
        make_frame(frame, (uint8_t)(i & 0xFF));
        rb.append(frame);
        TEST_ASSERT_TRUE(rb.bytesStored() <= kFrames * kFrameSize);
    }

    TEST_ASSERT_EQUAL_UINT32(kFrames * kFrameSize, rb.bytesStored());
    TEST_ASSERT_EQUAL_UINT32(rb.bytesStored(), rb.plan().totalBytes());
}

// A22 — the payload stays a whole number of frames across every wrap.
//
// This is R13 stated as an invariant rather than as an arithmetic fact. The ring is
// addressed in FRAMES, so no sequence of appends can leave a partial one — and the
// server slices the payload on a fixed 18-byte stride, so a buffer that ever held
// 699984 + 16 bytes would decode garbage from that point on and keep decoding
// garbage for the rest of the file.
static void test_bytes_stored_stays_a_whole_number_of_frames_across_a_wrap(void)
{
    RingBuffer rb(storage, kFrames, kFrameSize);

    uint8_t frame[kFrameSize];
    for (uint32_t i = 0; i < 4 * kFrames + 3; i++)
    {
        make_frame(frame, (uint8_t)(i & 0xFF));
        rb.append(frame);

        TEST_ASSERT_EQUAL_UINT32(0, rb.bytesStored() % kFrameSize);
        TEST_ASSERT_EQUAL_UINT32(0, rb.plan().totalBytes() % kFrameSize);
    }
}

static int run_all(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bytes_stored_stays_a_whole_number_of_frames_across_a_wrap);
    RUN_TEST(test_ring_capacity_is_whole_number_of_frames);
    RUN_TEST(test_append_advances_head_by_one_frame);
    RUN_TEST(test_plan_is_one_range_from_zero_when_not_wrapped);
    RUN_TEST(test_plan_is_two_ranges_starting_at_tail_when_wrapped);
    RUN_TEST(test_append_overwrites_oldest_frame_when_full);
    RUN_TEST(test_bytes_stored_never_exceeds_capacity);
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
