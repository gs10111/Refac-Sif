#ifndef THROUGHPUT_H
#define THROUGHPUT_H

#include <stdint.h>

// Bits per second, in kilobits, over a span in milliseconds.
//
// It exists as a function because the first version of this number was computed
// inline over the WRONG span: it divided the payload by connect + write + the two
// seconds spent waiting for the server's reply. A 159 ms transmission reported as
// 16.82 kbps, and that number sent a bench session chasing a slow link that was
// never slow. The span is now the caller's to state, and this only divides.
float kbps_from(uint32_t bytes, uint32_t elapsedMs);

#endif // THROUGHPUT_H
