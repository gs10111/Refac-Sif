#include "throughput.h"

float kbps_from(uint32_t bytes, uint32_t elapsedMs)
{
    if (elapsedMs == 0)
        return 0.0f;                 // a span of zero is not infinite speed
    return ((float)bytes * 8.0f) / (float)elapsedMs;
}
