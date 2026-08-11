#ifndef ICM42688P_H
#define ICM42688P_H
#include "Arduino.h"
#include "SPI.h"

#include "imu.h"

enum AccelRange : uint8_t
{
    ACCEL_RANGE_16G = 0,
    ACCEL_RANGE_8G,
    ACCEL_RANGE_4G,
    ACCEL_RANGE_2G
};

enum OdrFreq : uint8_t
{
    ODR_32KHZ = 1,
    ODR_16KHZ,
    ODR_8KHZ,
    ODR_4KHZ,
    ODR_2KHZ,
    ODR_1KHZ,
    ODR_200HZ,
    ODR_100HZ,
    ODR_50HZ,
    ODR_25HZ,
    ODR_12HZ,
    ODR_6HZ,
    ODR_3HZ,
    ODR_1HZ,
    ODR_500HZ
};

enum Axis : uint8_t
{
    X_AXIS = 0,
    Y_AXIS,
    Z_AXIS
};
// The concrete driver behind IImu. It no longer owns storage: it fills the 14
// sensor bytes of a frame and returns. The ring buffer and the 4-byte timestamp
// belong to AcquisitionService, which is where they can be tested on a host.
class ICM42688P : public IImu
{
public:
    ICM42688P(SPIClass &spi, int8_t cs_pin, AccelRange range, OdrFreq odr);

    bool begin(int8_t sck, int8_t miso, int8_t mosi);

    // IImu
    void wake() override;
    void setSamplingCode(uint8_t odrNibble) override;
    void sleep() override;
    ImuStatus readSensorFrame(uint8_t *out) override;

    void wakeup(AccelRange range, OdrFreq odr);
    int16_t readTemperature();

private:
    SPIClass *_spi;
    int8_t _cs_pin;
    AccelRange _range;
    OdrFreq _odr;

    void spi_write(uint8_t reg, const uint8_t *data, uint32_t len);
    void spi_read(uint8_t reg, uint8_t *data, uint32_t len);
};

#define PACKET_FORMAT_2_SIZE 16  // Header (1b) + Accel (6b) + Reserved (6b) + Timestamp (2b) + Temp (1b)
#define PACKET_ACCEL_X_OFFSET 0
#define PACKET_TIMESTAMP_OFFSET 13
#define INVALID_SAMPLE_VALUE 0
#define MAX_SAMPLING_MICROS_TO_FAULT 1000000

/**
 * @brief Configuration registers
 *
 */

/*  FIFO_CONFIG1
    7 - Reserved
    6 FIFO_RESUME_PARTIAL_RD
        0: Partial FIFO read disabled, requires re-reading of the entire FIFO
        1: FIFO read can be partial, and resume from last read point
    5 FIFO_WM_GT_TH Trigger FIFO watermark interrupt on every ODR (DMA write) if FIFO_COUNT ≥ FIFO_WM_TH
    4 FIFO_HIRES_EN
        0: Default setting; Sensor data have regular resolution
        1: Sensor data in FIFO will have extended resolution enabling the 20 Bytes packet
    3 FIFO_TMST_FSYNC_EN Must be set to 1 for all FIFO use cases when FSYNC is used
    2 FIFO_TEMP_EN Enable temperature sensor packets to go to FIFO
    1 Reserved 0
    0 FIFO_ACCEL_EN Enable accelerometer packets to go to FIFO
*/
#define FIFO_CONFIG1_REG 0x5F
#define FIFO_CONFIG1_SETUP 0b00000101

/*
    INTF_CONFIG0
    7 FIFO_HOLD_LAST_DATA_EN
    Setting 0 corresponds to the following:
        Sense Registers from Power on Reset till first sample:
            • Invalid Samples Value: -32768 Sense Registers after first sample received:
            • Sense Registers Valid Sample Values:
                o Range limited from -32766 to +32767 when FSYNC tag is disabled, or for sensor not selected for FSYNC tag
                o Range limited from -32765 to +32767 (odd values) for sensor selected for FSYNC tag, and FSYNC is tagged
                o Range limited from -32766 to +32766 (even values) for sensor selected for FSYNC tag, but FSYNC is not tagged
            • Sense Registers Invalid Sample Values:
                o -32768 when FSYNC tag is disabled, or for sensor not selected for
                FSYNC tag, or for sensor selected for FSYNC tag but FSYNC is not
                tagged
                o -32767 for sensor selected for FSYNC tag, and FSYNC is tagged
        FIFO:
            • Invalid Sample Value: -32768
            • Valid Sample Values: -32766 to +32767
    Setting 1 corresponds to the following:
        Sense Registers from Power on Reset till first sample:
            • Invalid Samples Value: 0
            Sense Registers after first sample received:
            • Sense Registers Valid Sample Values:
                o Range limited from -32768 to +32767 when FSYNC tag is disabled,
                or for sensor not selected for FSYNC tag
                o Range limited from -32767 to +32767 (odd values) for sensor
                selected for FSYNC tag, and FSYNC is tagged
                o Range limited from -32768 to +32766 (even values) for sensor
                selected for FSYNC tag, but FSYNC is not tagged
            • Sense Registers Invalid Sample Values:
                o Registers hold last valid sample until new one arrives
        FIFO:
            • Invalid Sample Value: Copy last valid sample
            • Valid Sample Values: -32768 to +32767
    6 FIFO_COUNT_REC
        0: FIFO count is reported in bytes
        1: FIFO count is reported in records (1 record = 16 bytes for header + accel + temp sensor data + time stamp, or 8 bytes for header + accel + temp sensor data)
    5 FIFO_COUNT_ENDIAN
        0: FIFO count is reported in Little Endian format
        1: FIFO count is reported in Big Endian format (default)
    4 SENSOR_DATA_ENDIAN
        0: Sensor data is reported in Little Endian format
        1: Sensor data is reported in Big Endian format (default)
    3:2 - Reserved
    1:0 UI_SIFS_CFG
        0x: Reserved
        10: Disable SPI
        11: Disable I2C
*/
#define INTF_CONFIG0_REG 0x4C
#define INTF_CONFIG0_SETUP 0b00110000

/*
IIM-42352
Page 71 of 101
Document Number: DS-000442
Revision: 1.4
14.32 ACCEL_CONFIG0
Name: ACCEL_CONFIG0
Address: 80 (50h)
Serial IF: R/W
Reset value: 0x03
Clock Domain: SCLK_UI
BIT NAME FUNCTION
7:5 ACCEL_FS_SEL
Full scale select for accelerometer UI interface output
000: ±16g (default)
001: ±8g
010: ±4g
011: ±2g
100: Reserved
101: Reserved
110: Reserved
111: Reserved
4 Reserved Reserved
3:0 ACCEL_ODR
Accelerometer ODR selection for UI interface output
0000: Reserved
0001: 32kHz
0010: 16kHz
0011: 8kHz (LN mode)
0100: 4kHz (LN mode)
0101: 2kHz (LN mode)
0110: 1kHz (LN mode) (default)
0111: 200Hz (LP or LN mode)
1000: 100Hz (LP or LN mode)
1001: 50Hz (LP or LN mode)
1010: 25Hz (LP or LN mode)
1011: 12.5Hz (LP or LN mode)
1100: 6.25Hz (LP mode)
1101: 3.125Hz (LP mode)
1110: 1.5625Hz (LP mode)
1111: 500Hz (LP or LN mode)
*/
#define ACCEL_CONFIG0_REG 0x50
#define ACCEL_CONFIG0_SETUP 0b00100001
#define ACCEL_FILT_CONFIG_REG 0x52
#define ACCEL_FILT_CONFIG_SETUP_1BW 0b00000001
#define ACCEL_FILT_CONFIG_SETUP_2BW 0b00000010
#define ACCEL_FILT_CONFIG_SETUP_3BW 0b00000011

#define GYRO_CONFIG0_REG 0x4F
#define GYRO_CONFIG0_SETUP 0b00001001  // 3:0 GYRO_ODR 7:5 GYRO_FS_SEL

#define INTF_CONFIG1_REG 0x4D
#define INTF_CONFIG1_SETUP 0b00001000
#define INTF_CONFIG1_RESET 0b00001011

#define INTF_CONFIG5_REG 0x7B
#define INTF_CONFIG5_SETUP 0b00000000

#define PWR_MGMT0_REG 0x4E
#define PWR_MGMT0_SETUP_LN 0b00011111  // 3:2 GYRO_MODE  1:0 ACCEL_MODE
#define PWR_MGMT0_SETUP_LP 0b00010010
#define PWR_MGMT0_RESET 0b00100000

#define FIFO_CONFIG_REG 0x16

// Data regs
#define INT_STATUS_REG 0x2D
#define FIFO_COUNT_H_REG 0x2E
#define FIFO_COUNT_L_REG 0x2F
#define ACCEL_DATA_X1_REG 0x1F
#define ACCEL_DATA_X0_REG 0x20
#define ACCEL_DATA_Y1_REG 0x21
#define ACCEL_DATA_Y0_REG 0x22
#define ACCEL_DATA_Z1_REG 0x23
#define ACCEL_DATA_Z0_REG 0x24
#define GYRO_DATA_X1_REG 0x25
#define GYRO_DATA_X0_REG 0x26
#define GYRO_DATA_Y1_REG 0x27
#define GYRO_DATA_Y0_REG 0x28
#define GYRO_DATA_Z1_REG 0x29
#define GYRO_DATA_Z0_REG 0x2A
#define TEMP_DATA1_REG 0x1D  // (TEMP_DATA / 132.48) + 25
#define TEMP_DATA0_REG 0x1E
#define WHO_AM_I_REG 0x75
#define FIFO_DATA_REG 0x30

// PINS
#define PU_PD_CONFIG2 0x0E
#define PU_PD_CONFIG2_RESET 0b01010101
#define PU_PD_CONFIG1 0x06
#define PU_PD_CONFIG1_RESET 0b00010001

// Masks
#define DATA_RDY_INT_MASK 0b00001000
#define FIFO_FULL_INT_MASK 0b00000010
#define FIFO_IS_EMPTY_MASK 0b10000000
#define FIFO_IS_PACKET_2_FORMATED_MASK 0b01100000
#define FIFO_PACKET_CONTAINS_ODR_TIMESTAMP_MASK 0b00001000

// APEX
#define APEX_CONFIG0 0x56
#define APEX_CONFIG0_RESET 0b00000000

// BANK SEL
#define REG_BANK_SEL 0x76
#define SEL_BANK_0 0b00000000
#define SEL_BANK_1 0b00000001
#define SEL_BANK_2 0b00000010
#define SEL_BANK_3 0b00000011
#define SEL_BANK_4 0b00000100

#endif // ICM42688P_H