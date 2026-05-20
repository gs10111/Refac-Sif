#ifndef PACKET_H
#define PACKET_H

#include <stdint.h>

// Wire format constants
#define SAMPLE_SIZE_BYTES    18
#define HEADER_SIZE_BYTES     4   // uint32_t total_bytes sent before data
#define BATTERY_SIZE_BYTES    2   // uint16_t sent after all samples

// Per-sample layout — 18 bytes, little-endian
// [0-3]   timestamp_ms  uint32_t
// [4-5]   accel_x       int16_t
// [6-7]   gyro_x        int16_t
// [8-9]   accel_y       int16_t
// [10-11] gyro_y        int16_t
// [12-13] accel_z       int16_t
// [14-15] gyro_z        int16_t
// [16-17] temperature   int16_t

// Server → firmware config (8 bytes, little-endian)
struct ServerConfig {
    uint16_t sleep_time_min;       // minutes to sleep between cycles
    uint16_t idle_timeout_min;     // minutes without trigger before sleeping
    uint16_t max_acquisitions;     // acquisition cycles before sleeping
    uint16_t trigger_cooldown_sec; // seconds between triggers
} __attribute__((packed));

// Default config used before first server contact
#define DEFAULT_SLEEP_TIME_MIN       240
#define DEFAULT_IDLE_TIMEOUT_MIN      20
#define DEFAULT_MAX_ACQUISITIONS       5
#define DEFAULT_TRIGGER_COOLDOWN_SEC   5

#endif // PACKET_H
