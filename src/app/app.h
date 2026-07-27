#ifndef app_h
#define app_h

#include "board.h"
#include "ICM42688P.h"
#include "acquisition.h"
#include "sample_store.h"
#include "belt_cycle.h"
#include "uploader.h"
#include "ota_arming.h"
#include "cycle_runner.h"
#include "belt_trigger.h"
#include "ring_buffer.h"
#include "../services/esp32_platform.h"
#include "power_manager.h"
#include "../services/connectivity/wifi_manager.h"
#include "../services/connectivity/tcp_client.h"

class App {
public:
    void begin();
    void run();

private:
    void runOtaWindow();

    SPIClass            _spi{HSPI};
    ICM42688P           _imu{_spi, IMU_CS_PIN, ACCEL_RANGE_16G, ODR_50HZ};
    PsramAllocator      _alloc;
    HaltFault           _fault;
    RingBuffer         *_ring      = nullptr;
    AcquisitionService *_acq       = nullptr;
    BeltTrigger         _trigger;
    WiFiManager         _wifi;
    EspCpu              _cpu;
    EspSleeper          _sleeper;
    // Declared after its collaborators: member initialisation follows declaration
    // order, and these references must be bound to live objects.
    PowerManager        _power{_imu, _wifi, _cpu, _sleeper};
    TcpClient           _tcp;
    NvsStore            _store;
    EspAccessPoint      _ap;
    EspClock            _clock;
    GpioTriggerSource   _pin;
    AdcBatterySense     _battery;

    // Built in begin(), once the ring and the acquisition service exist.
    CycleRunner        *_cycle = nullptr;

    // Plain RAM, and that is correct: they describe the cycle currently in
    // progress, and a cycle never spans a reset. D1 keeps the device awake between
    // acquisitions, so nothing here has to survive one.
    bool _idleTimedOut   = false;
    bool _updateRequested = false;
};

#endif // app_h
