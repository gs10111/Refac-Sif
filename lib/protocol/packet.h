#ifndef PACKET_H
#define PACKET_H

#include <stdint.h>

// Wire format constants
#define SAMPLE_SIZE_BYTES    18
#define HEADER_SIZE_BYTES     4   // uint32_t total_bytes sent before data
#define BATTERY_SIZE_BYTES    2   // uint16_t sent after all samples

#define FRAME_TIMESTAMP_BYTES 4   // leading uint32_t of every sample frame

// Per-sample layout — 18 bytes, little-endian
// [0-3]   timestamp_ms  uint32_t
// [4-5]   accel_x       int16_t
// [6-7]   gyro_x        int16_t
// [8-9]   accel_y       int16_t
// [10-11] gyro_y        int16_t
// [12-13] accel_z       int16_t
// [14-15] gyro_z        int16_t
// [16-17] temperature   int16_t

// Server → firmware config — 12 bytes, 6 x uint16 little-endian:
//   offset 0: sleep_min      offset 2: idle_min       offset 4: max_acq
//   offset 6: cooldown_sec   offset 8: update         offset 10: sampling_code
// Field order is the wire order. The first five fields keep the offsets the
// legacy server packs with '<HHHHH', so nothing about them moved; the rate was
// appended. A legacy 10-byte response is REFUSED rather than half-applied — a
// device fed by an old server keeps the config it already has. Do not reorder.
#define SERVER_CONFIG_WIRE_BYTES 12

struct ServerConfig {
    uint16_t sleep_time_min;       // minutes to sleep between cycles
    uint16_t idle_timeout_min;     // minutes without trigger before sleeping
    uint16_t max_acquisitions;     // acquisition cycles before sleeping
    uint16_t trigger_cooldown_sec; // seconds between triggers
    uint16_t update;               // 1 arms the one-shot OTA on the next boot
    uint16_t sampling_code;        // ODR nibble for ACCEL_CONFIG0/GYRO_CONFIG0
} __attribute__((packed));

static_assert(sizeof(ServerConfig) == SERVER_CONFIG_WIRE_BYTES,
              "ServerConfig must match the 12-byte wire layout");

// ODR nibbles of the ICM-42688-P, the only five where accelerometer AND
// gyroscope both run: 7 = 200 Hz, 8 = 100, 9 = 50, 10 = 25, 11 = 12.5.
// Codes 12-14 are Reserved for the gyroscope on this part — not a low-power
// accelerometer mode to be discovered later. 15 (500 Hz) works on both and is
// left out by scope. 0 is the server saying it has no opinion about the rate.
#define SAMPLING_CODE_200HZ   7
#define SAMPLING_CODE_100HZ   8
#define SAMPLING_CODE_50HZ    9
#define SAMPLING_CODE_25HZ   10
#define SAMPLING_CODE_12HZ5  11

// The rate the fleet ran before the rate was configurable, and the rate a
// device falls back to when what it reads is not one it can run.
#define DEFAULT_SAMPLING_CODE SAMPLING_CODE_50HZ

// NVS key holding the rate between boots. Short on purpose: NVS keys are capped
// at 15 characters, and this one is read on every boot before the radio is up.
#define SAMPLING_CODE_NVS_KEY "freq"

// True only for the five codes above. Every path that could put a nibble into
// ACCEL_CONFIG0 goes through here: what the server sends and what NVS returns
// are both untrusted, and a Reserved nibble reaching the register configures
// the part into a mode the datasheet does not define.
bool is_valid_sampling_code(uint16_t code);

// The code to actually run, given whatever was stored. Anything unusable —
// never written, corrupt, a rate the part cannot do — reads as 50 Hz.
uint8_t sampling_code_or_default(uint16_t stored);

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
