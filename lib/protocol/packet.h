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

// Server → firmware config — 10 bytes, 5 x uint16 little-endian:
//   offset 0: sleep_min      offset 2: idle_min       offset 4: max_acq
//   offset 6: cooldown_sec   offset 8: update
// Field order is the wire order. It keeps the legacy server, which packs
// '<HHHHH', compatible with this firmware. Do not reorder.
#define SERVER_CONFIG_WIRE_BYTES 10

struct ServerConfig {
    uint16_t sleep_time_min;       // minutes to sleep between cycles
    uint16_t idle_timeout_min;     // minutes without trigger before sleeping
    uint16_t max_acquisitions;     // acquisition cycles before sleeping
    uint16_t trigger_cooldown_sec; // seconds between triggers
    uint16_t update;               // 1 arms the one-shot OTA on the next boot
} __attribute__((packed));

static_assert(sizeof(ServerConfig) == SERVER_CONFIG_WIRE_BYTES,
              "ServerConfig must match the 10-byte wire layout");

// Decode a server response frame. Requires the full 10 bytes; leaves `out`
// untouched when the frame is short, so a truncated response can never
// half-reconfigure the duty cycle.
bool parse_server_config(const uint8_t *bytes, uint32_t len, ServerConfig &out);

// Config used before the first server contact.
ServerConfig default_server_config();

// Default config used before first server contact
#define DEFAULT_SLEEP_TIME_MIN       240
#define DEFAULT_IDLE_TIMEOUT_MIN      20
#define DEFAULT_MAX_ACQUISITIONS       5
#define DEFAULT_TRIGGER_COOLDOWN_SEC   5
#define DEFAULT_UPDATE                 0

#endif // PACKET_H
