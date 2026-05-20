#include "acquisition.h"
#include "Arduino.h"

AcquisitionService::AcquisitionService(ICM42688P &driver,
                                       uint8_t *buf,
                                       uint32_t bufSize)
    : _driver(driver),
      _buf(buf),
      _bufSize(bufSize),
      _stopFlag(false),
      _deepSleepFlag(false),
      _acquisitionCount(0)
{
    _config = {
        DEFAULT_SLEEP_TIME_MIN,
        DEFAULT_IDLE_TIMEOUT_MIN,
        DEFAULT_MAX_ACQUISITIONS,
        DEFAULT_TRIGGER_COOLDOWN_SEC};
}

void AcquisitionService::setConfig(const ServerConfig &config)
{
    _config = config;
}

void AcquisitionService::collect()
{
    _stopFlag      = false;
    _deepSleepFlag = false;
    _driver.reset();
    _acquisitionCount++;

    uint64_t startTime = millis();
    uint64_t timeoutMs = (uint64_t)_config.idle_timeout_min * 60ULL * 1000ULL;

    _driver.wakeup(ACCEL_RANGE_16G, ODR_50HZ);

    while (!_stopFlag)
    {
        _driver.sampleOnceMEMS3Dbyte(_buf, _bufSize);

        if ((uint64_t)(millis() - startTime) > timeoutMs)
        {
            _deepSleepFlag = true;
            break;
        }
        if (_acquisitionCount > _config.max_acquisitions)
        {
            _deepSleepFlag = true;
            _acquisitionCount = 0;
            break;
        }
    }
}

void AcquisitionService::stop()
{
    _stopFlag = true;
}

bool AcquisitionService::shouldSleep()
{
    return _deepSleepFlag;
}

uint32_t AcquisitionService::bytesCollected()
{
    return _driver.head;
}
