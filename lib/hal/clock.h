#ifndef CLOCK_H
#define CLOCK_H

#include <stdint.h>

// Milliseconds since boot.
//
// Deliberately absent from every other unit in this codebase: BeltTrigger and
// AcquisitionService RECEIVE `now` as a parameter, so a clock interface there would
// have been ceremony. The acquisition loop is where millis() enters the system
// rather than being passed through it, which is the one place this earns itself.
class IClock
{
public:
    virtual ~IClock() {}
    virtual uint32_t millis() = 0;
};

#endif // CLOCK_H
