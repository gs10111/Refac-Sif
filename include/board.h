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
// Memory
// =============================================================================
// Size of the PSRAM acquisition buffer, in BYTES — the same ps_malloc size the
// production firmware uses.
//
// The unit in the name is not decoration. The original constant was 350000 and
// counted int16 WORDS (350000 * 2 = 700000 bytes); reading it as SAMPLES and
// multiplying by the 18-byte frame asked for 6.3 MB, which no ESP32 can map, so
// ps_malloc returned null and the first sample stored to address 0.
//
// 700000 is not a whole number of 18-byte frames, so the ring uses 38888 frames
// = 699984 bytes and the trailing 16 bytes are never written. That keeps the
// payload a whole number of frames and every wrap frame-aligned.
#define ACQUISITION_BUFFER_BYTES     700000UL
#define ACQUISITION_FRAME_CAPACITY   (ACQUISITION_BUFFER_BYTES / SAMPLE_SIZE_BYTES)
#define ACQUISITION_USABLE_BYTES     (ACQUISITION_FRAME_CAPACITY * SAMPLE_SIZE_BYTES)

static_assert(ACQUISITION_BUFFER_BYTES == 700000UL,
              "allocation must stay at the size production uses");
static_assert(ACQUISITION_FRAME_CAPACITY == 38888UL,
              "700000 / 18 must be 38888 whole frames");
static_assert(ACQUISITION_USABLE_BYTES == 38888UL * 18UL,
              "usable size must be a whole number of frames");

#endif // BOARD_H
