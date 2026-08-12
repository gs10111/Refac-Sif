#include "packet.h"

// Decoded field by field rather than by memcpy over the packed struct: the wire
// is defined as little-endian regardless of what the host is, and the host test
// build is not the ESP32.
bool parse_server_config(const uint8_t *bytes, uint32_t len, ServerConfig &out)
{
    if (bytes == nullptr || len < SERVER_CONFIG_WIRE_BYTES)
        return false;

    ServerConfig parsed;
    parsed.sleep_time_min       = (uint16_t)(bytes[0] | (bytes[1] << 8));
    parsed.idle_timeout_min     = (uint16_t)(bytes[2] | (bytes[3] << 8));
    parsed.max_acquisitions     = (uint16_t)(bytes[4] | (bytes[5] << 8));
    parsed.trigger_cooldown_sec = (uint16_t)(bytes[6] | (bytes[7] << 8));
    parsed.update               = (uint16_t)(bytes[8] | (bytes[9] << 8));
    parsed.sampling_code        = (uint16_t)(bytes[10] | (bytes[11] << 8));

    out = parsed;
    return true;
}

ServerConfig default_server_config()
{
    ServerConfig cfg;
    cfg.sleep_time_min       = DEFAULT_SLEEP_TIME_MIN;
    cfg.idle_timeout_min     = DEFAULT_IDLE_TIMEOUT_MIN;
    cfg.max_acquisitions     = DEFAULT_MAX_ACQUISITIONS;
    cfg.trigger_cooldown_sec = DEFAULT_TRIGGER_COOLDOWN_SEC;
    cfg.update               = DEFAULT_UPDATE;
    cfg.sampling_code        = DEFAULT_SAMPLING_CODE;
    return cfg;
}

bool is_valid_sampling_code(uint16_t code)
{
    return code == SAMPLING_CODE_200HZ || code == SAMPLING_CODE_100HZ ||
           code == SAMPLING_CODE_50HZ  || code == SAMPLING_CODE_25HZ  ||
           code == SAMPLING_CODE_12HZ5;
}

uint8_t sampling_code_or_default(uint16_t stored)
{
    return is_valid_sampling_code(stored) ? (uint8_t)stored
                                          : (uint8_t)DEFAULT_SAMPLING_CODE;
}

void pack_trailer(uint8_t *out, uint16_t batteryMv,
                  uint8_t major, uint8_t minor, uint8_t patch,
                  uint16_t effectiveHz)
{
    if (out == nullptr)
        return;
    out[0] = (uint8_t)(batteryMv & 0xFF);
    out[1] = (uint8_t)((batteryMv >> 8) & 0xFF);
    out[2] = major;
    out[3] = minor;
    out[4] = patch;
    out[5] = (uint8_t)(effectiveHz & 0xFF);
    out[6] = (uint8_t)((effectiveHz >> 8) & 0xFF);
}

void pack_trailer_for_this_build(uint8_t *out, uint16_t batteryMv,
                                 uint16_t effectiveHz)
{
    pack_trailer(out, batteryMv,
                 SIF_FW_VERSION_MAJOR, SIF_FW_VERSION_MINOR, SIF_FW_VERSION_PATCH,
                 effectiveHz);
}

uint16_t effective_hz(uint32_t frames, uint32_t elapsedMs)
{
    if (frames == 0 || elapsedMs == 0)
        return 0;

    // Rounded, not truncated: 62.7 Hz reported as 62 would read as a sensor
    // running slower than it is, every time, by less than one hertz.
    const uint64_t scaled = ((uint64_t)frames * 1000ULL + (elapsedMs / 2)) / elapsedMs;
    return scaled > 65535ULL ? (uint16_t)65535 : (uint16_t)scaled;
}
