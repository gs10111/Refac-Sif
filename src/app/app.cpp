#include "app.h"
#include "board.h"
#include "packet.h"
#include "belt_state_machine.h"
#include "Arduino.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "config/server_config.cpp"

RTC_DATA_ATTR static AcquisitionStage stage = STAGE_SLEEP_TIMER;
RTC_DATA_ATTR static ServerConfig serverConfig = {
    DEFAULT_SLEEP_TIME_MIN,
    DEFAULT_IDLE_TIMEOUT_MIN,
    DEFAULT_MAX_ACQUISITIONS,
    DEFAULT_TRIGGER_COOLDOWN_SEC};

void App::begin()
{
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // Desabilita brownout reset
    Serial.begin(115200);
    pinMode(MAGNET_TRIGGER_PIN, INPUT_PULLUP);
    pinMode(IMU_CS_PIN, OUTPUT);
    pinMode(BATTERY_ADC_PIN, INPUT);
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LOW);
    pinMode(IMU_INT1_PIN, INPUT);
    pinMode(IMU_INT2_PIN, INPUT);
    _wifi.configure(DEVICE_IP, NETWORK_GW, NETWORK_SUBNET);
    _sampleBuf = (uint8_t *)ps_malloc(ACQUISITION_BUFFER_BYTES);
    _imu.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN);
    _acq = new AcquisitionService(_imu, _sampleBuf, ACQUISITION_BUFFER_BYTES);
    _acq->setConfig(serverConfig);
    setCpuFrequencyMhz(10);
}

void App::run()
{
    switch (stage)
    {

    case STAGE_SLEEP_TIMER:
        stage = STAGE_WAIT_TRIGGER;
        _power.sleepUntilTrigger();
        break;

    case STAGE_WAIT_TRIGGER:
        stage = STAGE_ACQUIRE_TRANSMIT;
        _acq->collect();

        setCpuFrequencyMhz(240);
        if (_wifi.connect(WIFI_SSID, WIFI_PASSWORD))
        {
            if (_tcp.connect(SERVER_HOST, TCP_SERVER_PORT))
            {
                uint16_t batt = _power.readBatteryMv();
                ServerConfig newConfig;
                if (_tcp.sendData(_sampleBuf, _acq->bytesCollected(), batt, newConfig))
                {
                    serverConfig = newConfig;
                    _acq->setConfig(serverConfig);
                }
                _tcp.disconnect();
            }
            _wifi.disconnect();
        }
        setCpuFrequencyMhz(10);

        if (_acq->shouldSleep())
        {
            stage = STAGE_SLEEP_TIMER;
            _power.sleepTimer(serverConfig.sleep_time_min);
        }
        else
        {
            stage = STAGE_WAIT_TRIGGER;
            _power.sleepUntilTrigger();
        }
        break;

    case STAGE_ACQUIRE_TRANSMIT:
        break;
    }
}
