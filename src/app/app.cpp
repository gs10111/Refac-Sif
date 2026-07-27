#include "app.h"
#include "board.h"
#include "packet.h"
#include "Arduino.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "config/server_config.cpp"

// Survives a deep-sleep wake; reloaded from flash on every other reset, which is
// why BELT_INITIAL_STAGE has to be the safe state rather than a resume point.
RTC_DATA_ATTR static BeltStage stage = BELT_INITIAL_STAGE;
RTC_DATA_ATTR static ServerConfig serverConfig = {
    DEFAULT_SLEEP_TIME_MIN,
    DEFAULT_IDLE_TIMEOUT_MIN,
    DEFAULT_MAX_ACQUISITIONS,
    DEFAULT_TRIGGER_COOLDOWN_SEC,
    DEFAULT_UPDATE};

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
    _ring = new RingBuffer(_sampleBuf, ACQUISITION_FRAME_CAPACITY, SAMPLE_SIZE_BYTES);
    _acq = new AcquisitionService(_imu, *_ring);
    _acq->setConfig(serverConfig);

    _imu.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN);
    _imu.wake(); // once per boot, as main.cpp:114 does — not per acquisition

    // Starts the initial cooldown, which is what keeps the magnet that caused the
    // ext0 wake from ending the first acquisition on its first poll.
    _trigger.begin(millis(), serverConfig.trigger_cooldown_sec);

    setCpuFrequencyMhz(10);
}

void App::run()
{
    BeltInputs in;
    in.otaFlagSet = false; // NVS-backed OTA arming lands in round 8
    in.acquisitionsDone = _acq->acquisitionsAttempted();
    in.maxAcquisitions = serverConfig.max_acquisitions;
    in.idleTimedOut = _idleTimedOut;
    in.updateRequestedByServer = _updateRequested;

    BeltStep step = belt_next(stage, in);
    stage = step.next;

    switch (step.action)
    {
    case BELT_ACTION_SLEEP_UNTIL_TRIGGER:
        _power.sleepUntilTrigger();
        break;

    case BELT_ACTION_RUN_CYCLE_ITERATION:
        runCycleIteration();
        break;

    case BELT_ACTION_SLEEP_TIMER:
        _acq->endCycle();
        _power.sleepTimer(serverConfig.sleep_time_min);
        break;

    case BELT_ACTION_ENTER_OTA:
    case BELT_ACTION_RESTART:
        // Both are unreachable this round: otaFlagSet is hardwired false above and
        // _updateRequested is never set until round 8 wires the NVS flag and the
        // SoftAP. They restart rather than fall through, because no path in this
        // firmware returns without an action — an empty case is what left the
        // refactor spinning with nothing to do.
        ESP.restart();
        break;

    case BELT_ACTION_INVALID:
    default:
        ESP.restart();
        break;
    }
}

void App::runCycleIteration()
{
    _acq->beginAcquisition(millis());

    AcquisitionResult result;
    do
    {
        TriggerLevel level = (digitalRead(MAGNET_TRIGGER_PIN) == LOW)
                                 ? TRIGGER_LEVEL_LOW
                                 : TRIGGER_LEVEL_HIGH;
        uint32_t now = millis();
        result = _acq->step(now, _trigger.poll(level, now));
    } while (result == ACQ_RUNNING);

    if (result == ACQ_STOPPED_BY_IDLE)
    {
        // The belt stopped. Production discards this acquisition unsent —
        // main.cpp:169 checks the flag before the transmit block and sleeps out of
        // loop() at :174 — and the ring is deliberately left alone, so whatever it
        // holds still goes out on the next successful send.
        _idleTimedOut = true;
        return;
    }

    setCpuFrequencyMhz(240);
    if (_wifi.connect(WIFI_SSID, WIFI_PASSWORD))
    {
        if (_tcp.connect(SERVER_HOST, TCP_SERVER_PORT))
        {
            // plan().first only. Correct and byte-identical to production while the
            // ring has not wrapped; a wrapped ring needs both ranges sent in order,
            // which is T45 in round 7.
            ReadPlan plan = _ring->plan();
            uint16_t batt = _power.readBatteryMv();
            ServerConfig newConfig;
            if (_tcp.sendData(plan.first.ptr, plan.first.len, batt, newConfig))
            {
                serverConfig = newConfig;
                _acq->setConfig(serverConfig);
                _trigger.setCooldownSec(serverConfig.trigger_cooldown_sec);
                // Only a successful send clears the buffer (main.cpp:264-265), so a
                // failed transmit carries its data into the next attempt.
                _ring->reset();
            }
            _tcp.disconnect();
        }
        _wifi.disconnect();
    }
    setCpuFrequencyMhz(10);
}
