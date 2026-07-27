#ifndef app_h
#define app_h

#include "board.h"
#include "ICM42688P.h"
#include "acquisition.h"
#include "sample_store.h"
#include "belt_cycle.h"
#include "belt_trigger.h"
#include "ring_buffer.h"
#include "../services/esp32_platform.h"
#include "../services/power_manager.h"
#include "../services/connectivity/wifi_manager.h"
#include "../services/connectivity/tcp_client.h"

class App {
public:
    void begin();
    void run();

private:
    void runCycleIteration();

    SPIClass            _spi{HSPI};
    ICM42688P           _imu{_spi, IMU_CS_PIN, ACCEL_RANGE_16G, ODR_50HZ};
    PsramAllocator      _alloc;
    HaltFault           _fault;
    RingBuffer         *_ring      = nullptr;
    AcquisitionService *_acq       = nullptr;
    BeltTrigger         _trigger;
    PowerManager        _power;
    WiFiManager         _wifi;
    TcpClient           _tcp;

    // Plain RAM, and that is correct: they describe the cycle currently in
    // progress, and a cycle never spans a reset. D1 keeps the device awake between
    // acquisitions, so nothing here has to survive one.
    bool _idleTimedOut   = false;
    bool _updateRequested = false;
};

#endif // app_h
