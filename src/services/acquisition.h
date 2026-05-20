#ifndef ACQUISITION_H
#define ACQUISITION_H

#include "ICM42688P.h"
#include "packet.h"

class AcquisitionService {
public:
    AcquisitionService(ICM42688P &driver, uint8_t *buf, uint32_t bufSize);

    void      setConfig(const ServerConfig &config);
    void      collect();   // blocking — runs until stop condition met
    void      stop();      // called from trigger task to end collection
    bool      shouldSleep();
    uint32_t  bytesCollected();

private:
    ICM42688P    &_driver;
    uint8_t      *_buf;
    uint32_t      _bufSize;
    ServerConfig  _config;
    volatile bool _stopFlag;
    bool          _deepSleepFlag;
    uint32_t      _acquisitionCount;
};

#endif // acquisition.h
