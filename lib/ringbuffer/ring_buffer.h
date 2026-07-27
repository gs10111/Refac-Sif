#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stdint.h>

// Frame-aligned circular buffer for acquisition samples.
//
// Capacity is expressed in FRAMES, never in bytes, so the usable size is always a
// whole number of 18-byte samples and every wrap lands on a frame boundary. The
// allocation itself stays at the size production uses (700000 B); the remainder
// below one frame is simply never written.
//
// Pure — no Arduino, no ESP headers. Host-testable.

struct ReadRange
{
    const uint8_t *ptr;
    uint32_t len;
};

// Transmission order for the stored frames, oldest first.
//   not wrapped -> `first` covers everything from index 0, `second.len` is 0.
//   wrapped     -> `first` starts at tail and runs to the end of the storage,
//                  `second` covers index 0 up to tail.
struct ReadPlan
{
    ReadRange first;
    ReadRange second;

    uint32_t totalBytes() const;
};

class RingBuffer
{
public:
    RingBuffer(uint8_t *storage, uint32_t frameCapacity, uint32_t frameSize);

    // How many whole frames fit in `bytes`. The remainder is unusable.
    static uint32_t frameCapacityFor(uint32_t bytes, uint32_t frameSize);

    // Store one frame, overwriting the oldest when full. False if storage is null.
    bool append(const uint8_t *frame);

    ReadPlan plan() const;
    uint32_t bytesStored() const;
    void reset();

private:
    uint8_t *_storage;
    uint32_t _frameCapacity;
    uint32_t _frameSize;
    uint32_t _head;  // frame index of the next write
    uint32_t _tail;  // frame index of the oldest stored frame
    uint32_t _count; // frames stored
};

#endif // RING_BUFFER_H
