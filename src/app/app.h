#ifndef app_h
#define app_h

#include "board.h"
#include "ICM42688P.h"
#include "../services/acquisition.h"
#include "../services/power_manager.h"
#include "../services/connectivity/wifi_manager.h"
#include "../services/connectivity/tcp_client.h"

class App {
public:
    void begin();
    void run();
private:
    SPIClass            _spi{HSPI};
    ICM42688P           _imu{_spi, IMU_CS_PIN};
    uint8_t            *_sampleBuf = nullptr;
    AcquisitionService *_acq       = nullptr;
    PowerManager        _power;
    WiFiManager         _wifi;
    TcpClient           _tcp;
};

#endif // app_h
