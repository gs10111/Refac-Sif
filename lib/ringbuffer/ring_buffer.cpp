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
    return bytes / frameSize;
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
