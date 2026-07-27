#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include <stdint.h>

// Injected so a test can make the allocation fail on demand. On the device this is
// ps_malloc; the acquisition buffer is far too large for internal RAM.
class IAllocator
{
public:
    virtual ~IAllocator() {}

    // Returns nullptr on failure. Callers must check — that omission is R1.
    virtual uint8_t *allocate(uint32_t bytes) = 0;
};

#endif // ALLOCATOR_H
