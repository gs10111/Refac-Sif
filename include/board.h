#ifndef BOARD_H
#define BOARD_H

// For SAMPLE_SIZE_BYTES — the acquisition buffer is sized in whole wire frames,
// so the memory constants below depend on the frame size.
#include "packet.h"

// =============================================================================
// SPI Bus
// =============================================================================
#define SPI_SCK_PIN     20
#define SPI_MISO_PIN     7
#define SPI_MOSI_PIN    19
#define IMU_CS_PIN       5

// =============================================================================
// IMU Interrupt Pins (ICM-42688-P INT1 / INT2)
// Currently defined but unused — future: interrupt-driven FIFO drain
// =============================================================================
#define IMU_INT1_PIN    35
#define IMU_INT2_PIN    34

// =============================================================================
// Belt Trigger
// Reed switch / Hall effect sensor — fires when belt magnet passes
// =============================================================================
#define MAGNET_TRIGGER_PIN  33

// =============================================================================
// Power Sensing
// ADC pin for battery voltage measurement
// =============================================================================
#define BATTERY_ADC_PIN     36
#define BATTERY_ADC_MAX   4095
#define BATTERY_MV_MAX   19803

// =============================================================================
// Status
// =============================================================================
#define STATUS_LED_PIN       2

// =============================================================================
// Network
// =============================================================================
#define TCP_SERVER_PORT  12345

// =============================================================================
// OTA
// SSID is built as "<prefix><MAC>", matching main.cpp:78 byte for byte.
// =============================================================================
#define OTA_AP_SSID_PREFIX  "Update driver - "
#define OTA_AP_PASSWORD     "12345678"
#define OTA_WINDOW_MINUTES  5

// =============================================================================
// Memory
// =============================================================================
// Size of the PSRAM acquisition buffer, in BYTES.
//
// The unit in the name is not decoration. The original constant was 350000 and
// counted int16 WORDS (350000 * 2 = 700000 bytes); reading it as SAMPLES and
// multiplying by the 18-byte frame asked for 6.3 MB, which no ESP32 can map, so
// ps_malloc returned null and the first sample stored to address 0.
//
// The size is derived, not chosen: the highest rate the device can be told to
// run (200 Hz) for the capture window the plant works with (5 minutes). At the
// 50 Hz the fleet ran before the rate became configurable, the same buffer
// holds 20 minutes — strictly more than the 777 s the previous 700000 bytes
// held, so no capture that fit before stops fitting.
//
// Unlike 700000, this size IS a whole number of 18-byte frames, so the ring
// spans the whole allocation and no trailing bytes go unused.
#define ACQUISITION_MAX_HZ           200UL
#define ACQUISITION_WINDOW_SECONDS   300UL
#define ACQUISITION_BUFFER_BYTES     (SAMPLE_SIZE_BYTES * ACQUISITION_MAX_HZ * ACQUISITION_WINDOW_SECONDS)
#define ACQUISITION_FRAME_CAPACITY   (ACQUISITION_BUFFER_BYTES / SAMPLE_SIZE_BYTES)
#define ACQUISITION_USABLE_BYTES     (ACQUISITION_FRAME_CAPACITY * SAMPLE_SIZE_BYTES)

static_assert(ACQUISITION_BUFFER_BYTES == 1080000UL,
              "200 Hz x 300 s x 18 B must be 1080000 bytes");
static_assert(ACQUISITION_FRAME_CAPACITY == 60000UL,
              "1080000 / 18 must be 60000 whole frames");
static_assert(ACQUISITION_USABLE_BYTES == ACQUISITION_BUFFER_BYTES,
              "the ring must span the whole allocation, with no unused tail");

#endif // BOARD_H
