#include "sample_store.h"

RingBuffer make_sample_ring(IAllocator &allocator,
                            IFault &fault,
                            uint32_t bytes,
                            uint32_t frameSize)
{
    uint8_t *storage = allocator.allocate(bytes);

#ifdef MUTANT_SKIP_ALLOCATION_CHECK
    // ==== MUTATION: MUTANT_SKIP_ALLOCATION_CHECK ====
    // Built only by [env:mutant_skip_allocation_check].
    // Never by env:native or env:pico32.
    // Run it:  pio test -e mutant_skip_allocation_check
    //
    // BREAKS: the returned pointer is not checked. The ring is built over whatever
    //         came back, at the full requested capacity — the refactor's actual
    //         behaviour restored (app.cpp:29 before round 5).
    // WHY:    it is R1 itself. On a module with less PSRAM than we believe, this is
    //         the difference between a device that says so and one that does not.
    // CAUGHT BY: test_sample_store test_allocation_failure_calls_fatal and
    //         test_failed_allocation_yields_a_ring_that_reports_no_capacity.
    // SURVIVED BY: test_no_sample_is_ever_written_when_allocation_failed — and that
    //         survival is the point. Nothing is written because RingBuffer refuses
    //         a null storage pointer, not because of the check removed here. The
    //         defence lives in the ring. A missing check therefore does not crash
    //         this firmware; it produces a device that boots, samples nothing and
    //         says nothing.
    (void)fault;
    return RingBuffer(storage, RingBuffer::frameCapacityFor(bytes, frameSize), frameSize);
#else
    if (storage == nullptr)
    {
        fault.fatal("Falha ao alocar memoria na PSRAM para o buffer de aquisicao");
        return RingBuffer(nullptr, 0, frameSize);
    }

    return RingBuffer(storage, RingBuffer::frameCapacityFor(bytes, frameSize), frameSize);
#endif
}
