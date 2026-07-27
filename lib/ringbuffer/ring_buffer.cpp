#include "ring_buffer.h"

#include <stddef.h>

uint32_t ReadPlan::totalBytes() const
{
    return first.len + second.len;
}

RingBuffer::RingBuffer(uint8_t *storage, uint32_t frameCapacity, uint32_t frameSize)
    : _storage(storage),
      _frameCapacity(frameCapacity),
      _frameSize(frameSize),
      _head(0),
      _tail(0),
      _count(0)
{
}

uint32_t RingBuffer::frameCapacityFor(uint32_t bytes, uint32_t frameSize)
{
    if (frameSize == 0)
        return 0;

#ifdef MUTANT_FRAME_CAPACITY_ROUNDS_UP
    // ==== MUTATION: MUTANT_FRAME_CAPACITY_ROUNDS_UP ====
    // Built only by [env:mutant_frame_capacity_rounds_up].
    // Never by env:native or env:pico32.
    // Run it:  pio test -e mutant_frame_capacity_rounds_up
    //
    // BREAKS: the capacity rounds UP, so the ring claims more bytes than it was
    //         given. 700000 / 18 becomes 38889 frames spanning 700002 bytes over a
    //         700000-byte ps_malloc — two bytes past the end of the allocation, on
    //         every wrap, forever. A heap overrun in PSRAM, silent and reachable.
    // WHY:    this is what CAN go wrong in the frame-addressed representation.
    //         R13's own defect — a byte modulus over 18-byte samples — has no local
    //         expression left, because the design deleted the byte modulus. When a
    //         fix is structural the defect stops having a mutation, and what you
    //         defend instead is whatever the new representation can get wrong.
    // CAUGHT BY: test_ringbuffer test_frame_capacity_never_spans_more_bytes_than_it_was_given
    //         and T06; test_sample_store T34, which reads the capacity through
    //         make_sample_ring.
    // SURVIVED BY: every test that constructs a ring with an explicit frame count,
    //         which is all the rest — they never call this function.
    return (bytes + frameSize - 1) / frameSize;
#else
    return bytes / frameSize;
#endif
}

bool RingBuffer::append(const uint8_t *frame)
{
    if (_storage == nullptr || frame == nullptr || _frameCapacity == 0)
        return false;

    uint8_t *dst = _storage + (size_t)_head * _frameSize;
    for (uint32_t i = 0; i < _frameSize; i++)
        dst[i] = frame[i];

    _head = (_head + 1) % _frameCapacity;

    if (_count == _frameCapacity)
        _tail = (_tail + 1) % _frameCapacity; // overwrote the oldest frame
    else
        _count++;

    return true;
}

ReadPlan RingBuffer::plan() const
{
    ReadPlan plan = {{nullptr, 0}, {nullptr, 0}};

    if (_storage == nullptr || _count == 0)
        return plan;

    // tail == 0 means the oldest frame already sits at index 0, so one range
    // covers everything. This is the path production takes, byte for byte.
    if (_tail == 0)
    {
        plan.first.ptr = _storage;
        plan.first.len = _count * _frameSize;
        return plan;
    }

    // Wrapped: oldest frame first, then the frames that overwrote the front.
    plan.first.ptr  = _storage + (size_t)_tail * _frameSize;
    plan.first.len  = (_frameCapacity - _tail) * _frameSize;
    plan.second.ptr = _storage;
    plan.second.len = _tail * _frameSize;
    return plan;
}

uint32_t RingBuffer::bytesStored() const
{
    return _count * _frameSize;
}

uint32_t RingBuffer::frameCapacity() const
{
    return _frameCapacity;
}

void RingBuffer::reset()
{
    _head = 0;
    _tail = 0;
    _count = 0;
}
