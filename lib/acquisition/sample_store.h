#ifndef SAMPLE_STORE_H
#define SAMPLE_STORE_H

#include <stdint.h>

#include "allocator.h"
#include "fault.h"
#include "ring_buffer.h"

// Allocates the acquisition buffer and wraps it in a frame-addressed ring.
//
// On failure it reports the fault and returns a ring of ZERO capacity over a null
// pointer — not the capacity that was asked for. A ring that claims storage it does
// not have is a lie every consumer downstream would believe, and making that state
// inexpressible is worth more than testing that nobody expresses it.
RingBuffer make_sample_ring(IAllocator &allocator,
                            IFault &fault,
                            uint32_t bytes,
                            uint32_t frameSize);

#endif // SAMPLE_STORE_H
