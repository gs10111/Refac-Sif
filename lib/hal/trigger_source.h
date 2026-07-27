#ifndef TRIGGER_SOURCE_H
#define TRIGGER_SOURCE_H

#include "belt_trigger.h"

// The magnet pin, and nothing more. Every judgement about what an edge MEANS lives
// in BeltTrigger, which is pure; this exists so the digitalRead has somewhere to be.
class ITriggerSource
{
public:
    virtual ~ITriggerSource() {}
    virtual TriggerLevel read() = 0;
};

#endif // TRIGGER_SOURCE_H
