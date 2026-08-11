#include "ICM42688P.h"
#include "Arduino.h"

#define SPI_READ 0x80
#define SPI_CLOCK 1000000 // 1 MHz — see known issue: max is 24 MHz
ICM42688P::ICM42688P(SPIClass &spi, int8_t cs_pin, AccelRange range, OdrFreq odr)
    : _spi(&spi), _cs_pin(cs_pin), _range(range), _odr(odr) {}

void ICM42688P::wake()
{
    wakeup(_range, _odr);
}

void ICM42688P::setSamplingCode(uint8_t odrNibble)
{
    // Stored, not written: ACCEL_CONFIG0 and GYRO_CONFIG0 are written by
    // wakeup(), while the sensors come out of OFF. Writing here would land on a
    // part that may be asleep, and be overwritten by the next wake() anyway.
    _odr = (OdrFreq)(odrNibble & 0x0F);
}
bool ICM42688P::begin(int8_t sck, int8_t miso, int8_t mosi)
{
    _spi->begin(sck, miso, mosi, _cs_pin);

    uint8_t pwrMgmt0 = PWR_MGMT0_RESET;
    uint8_t fifoConfig = 0b11000000;
    uint8_t fifoConfig1 = FIFO_CONFIG1_SETUP;
    uint8_t intfConfig0 = INTF_CONFIG0_SETUP;
    uint8_t intfConfig1 = INTF_CONFIG1_SETUP;
    uint8_t intfConfig5 = INTF_CONFIG5_SETUP;
    uint8_t filtConfig = ACCEL_FILT_CONFIG_SETUP_1BW;

    spi_write(PWR_MGMT0_REG, &pwrMgmt0, 1);
    spi_write(FIFO_CONFIG_REG, &fifoConfig, 1);
    spi_write(FIFO_CONFIG1_REG, &fifoConfig1, 1);
    spi_write(INTF_CONFIG0_REG, &intfConfig0, 1);
    spi_write(INTF_CONFIG1_REG, &intfConfig1, 1);
    spi_write(INTF_CONFIG5_REG, &intfConfig5, 1);
    spi_write(ACCEL_FILT_CONFIG_REG, &filtConfig, 1);

    return true;
}
void ICM42688P::wakeup(AccelRange range, OdrFreq odr)
{
    uint8_t pwrMgmt0 = PWR_MGMT0_SETUP_LN;
    uint8_t accelCfg = ((range & 0x07) << 5) | (odr & 0x0F);
    uint8_t gyroCfg  = (odr & 0x0F);
    uint8_t intfCfg0 = INTF_CONFIG0_SETUP;
    uint8_t intfCfg1 = INTF_CONFIG1_SETUP;
    uint8_t intfCfg5 = INTF_CONFIG5_SETUP;
    uint8_t filtCfg  = ACCEL_FILT_CONFIG_SETUP_1BW;

    spi_write(PWR_MGMT0_REG,         &pwrMgmt0, 1);
    spi_write(ACCEL_CONFIG0_REG,     &accelCfg, 1);
    spi_write(GYRO_CONFIG0_REG,      &gyroCfg,  1);
    spi_write(INTF_CONFIG0_REG,      &intfCfg0, 1);
    spi_write(INTF_CONFIG1_REG,      &intfCfg1, 1);
    spi_write(INTF_CONFIG5_REG,      &intfCfg5, 1);
    spi_write(ACCEL_FILT_CONFIG_REG, &filtCfg,  1);

    delay(45); // gyro startup per datasheet
}

void ICM42688P::sleep()
{
    uint8_t pwrMgmt0      = PWR_MGMT0_RESET;
    uint8_t pu_pd1        = PU_PD_CONFIG1_RESET;
    uint8_t pu_pd2        = PU_PD_CONFIG2_RESET;
    uint8_t bank0         = SEL_BANK_0;
    uint8_t bank3         = SEL_BANK_3;

    spi_write(REG_BANK_SEL,  &bank3,   1);
    delay(10);
    spi_write(PU_PD_CONFIG1, &pu_pd1,  1);
    spi_write(PU_PD_CONFIG2, &pu_pd2,  1);
    spi_write(REG_BANK_SEL,  &bank0,   1);
    delay(10);
    spi_write(PWR_MGMT0_REG, &pwrMgmt0, 1);
}
// Fills the 14 sensor bytes of a wire frame and returns. No storage, no
// timestamp, no modulo arithmetic — those moved to RingBuffer and
// AcquisitionService, where a host build can reach them. The byte order below is
// the wire order and matches production (ICM42688P.cpp:383-425 in the original):
// low byte then high byte, accel then gyro, X then Y then Z, temperature last.
ImuStatus ICM42688P::readSensorFrame(uint8_t *out)
{
    uint8_t intStatus = 0;
    spi_read(INT_STATUS_REG, &intStatus, 1);

    if (!(intStatus & DATA_RDY_INT_MASK))
        return IMU_NOT_READY;

    spi_read(ACCEL_DATA_X0_REG, &out[0], 1);
    spi_read(ACCEL_DATA_X1_REG, &out[1], 1);
    spi_read(GYRO_DATA_X0_REG, &out[2], 1);
    spi_read(GYRO_DATA_X1_REG, &out[3], 1);
    spi_read(ACCEL_DATA_Y0_REG, &out[4], 1);
    spi_read(ACCEL_DATA_Y1_REG, &out[5], 1);
    spi_read(GYRO_DATA_Y0_REG, &out[6], 1);
    spi_read(GYRO_DATA_Y1_REG, &out[7], 1);
    spi_read(ACCEL_DATA_Z0_REG, &out[8], 1);
    spi_read(ACCEL_DATA_Z1_REG, &out[9], 1);
    spi_read(GYRO_DATA_Z0_REG, &out[10], 1);
    spi_read(GYRO_DATA_Z1_REG, &out[11], 1);
    spi_read(TEMP_DATA0_REG, &out[12], 1);
    spi_read(TEMP_DATA1_REG, &out[13], 1);

    return IMU_OK;
}
int16_t ICM42688P::readTemperature()
{
    uint8_t h, l;
    spi_read(TEMP_DATA1_REG, &h, 1);
    spi_read(TEMP_DATA0_REG, &l, 1);
    return (int16_t)((h << 8) | l);
}

void ICM42688P::spi_write(uint8_t reg, const uint8_t *data, uint32_t len)
{
    _spi->beginTransaction(SPISettings(SPI_CLOCK, MSBFIRST, SPI_MODE0));
    digitalWrite(_cs_pin, LOW);
    _spi->transfer(reg);
    for (uint32_t i = 0; i < len; i++)
        _spi->transfer(data[i]);
    digitalWrite(_cs_pin, HIGH);
    _spi->endTransaction();
}

void ICM42688P::spi_read(uint8_t reg, uint8_t *data, uint32_t len)
{
    _spi->beginTransaction(SPISettings(SPI_CLOCK, MSBFIRST, SPI_MODE0));
    digitalWrite(_cs_pin, LOW);
    _spi->transfer(reg | SPI_READ);
    _spi->transfer(data, len);
    digitalWrite(_cs_pin, HIGH);
    _spi->endTransaction();
}
