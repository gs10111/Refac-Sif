#include "app.h"
#include "board.h"
#include "packet.h"
#include "Arduino.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "config/network_config.h"

// Survives a deep-sleep wake; reloaded from flash on every other reset, which is
// why BELT_INITIAL_STAGE has to be the safe state rather than a resume point.
RTC_DATA_ATTR static BeltStage stage = BELT_INITIAL_STAGE;

// Plain RAM, NOT RTC memory — deliberately, and it is the only variable here that
// should not survive a deep sleep.
//
// Production keeps the server response in an ordinary member of the IMU object
// (ICM42688P.h:257-264), so a timer wake starts from the compiled defaults and the
// server re-states the config on the next transmission. DEC-0 settles it on its
// own, but fidelity and safety agree here, which is not always true: a device that
// was handed a bad sleep_min self-heals on its next wake, whereas persisting it
// would put a device that took sleep_min = 1 — or 1440 — permanently out of reach
// of the fix.
//
// Within one wake it still carries across acquisitions, because D1 keeps the device
// awake for the whole cycle and nothing here is reinitialised until a reset.
static ServerConfig serverConfig = {
    DEFAULT_SLEEP_TIME_MIN,
    DEFAULT_IDLE_TIMEOUT_MIN,
    DEFAULT_MAX_ACQUISITIONS,
    DEFAULT_TRIGGER_COOLDOWN_SEC,
    DEFAULT_UPDATE};

void App::begin()
{
    Serial.begin(115200);

    // Before anything else, as production does at main.cpp:73 — the flag is read
    // and acted on ahead of pin setup and the acquisition path, which is also what
    // guarantees no transmission can intervene between a crash here and the retry.
    const OtaEntry ota = enter_ota_if_armed(_ap, _store, OTA_AP_SSID_PREFIX, OTA_AP_PASSWORD);
    if (ota == OTA_SERVING)
    {
        runOtaWindow(); // never returns — restarts when the window closes
    }
    else if (ota == OTA_BRINGUP_FAILED)
    {
        // The flag is still set. What happens next, in order, because two clauses
        // side by side were read as alternatives:
        //
        //   1. This boot carries on with an ordinary cycle. It does NOT transmit:
        //      ESP.restart() reloaded .rtc.data, so `stage` is back at ARM_TRIGGER
        //      and run() deep-sleeps waiting for the magnet.
        //   2. The next magnet wake retries the bring-up — the flag survived and
        //      begin() reads it again.
        //   3. Only if THAT fails does the cycle proceed to a transmission, whose
        //      update=0 disarms the flag. The request then ends visibly instead of
        //      latching until some future wake when an unattended AP appears.
        //
        // Step 1 depends on RTC_DATA_ATTR being reloaded on a software reset. That
        // is documented behaviour, not something anyone here has observed, and it is
        // on the HIL list. If it turns out RTC memory survives ESP.restart(), step 1
        // is wrong: `stage` would still be CYCLE, this boot would transmit, and the
        // retry at step 2 would never happen.
        Serial.println("OTA: falha ao subir o AP, tentando no proximo boot");
        _ap.stop();
    }

    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // Desabilita brownout reset
    pinMode(MAGNET_TRIGGER_PIN, INPUT_PULLUP);
    pinMode(IMU_CS_PIN, OUTPUT);
    pinMode(BATTERY_ADC_PIN, INPUT);
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LOW);
    pinMode(IMU_INT1_PIN, INPUT);
    pinMode(IMU_INT2_PIN, INPUT);
    _wifi.configure(DEVICE_IP, NETWORK_GW, NETWORK_SUBNET);

    // Allocates, checks, and halts loudly if PSRAM cannot give us the buffer. The
    // unchecked ps_malloc this replaces is R1.
    _ring = new RingBuffer(make_sample_ring(_alloc, _fault,
                                            ACQUISITION_BUFFER_BYTES,
                                            SAMPLE_SIZE_BYTES));
    _acq = new AcquisitionService(_imu, *_ring);
    _acq->setConfig(serverConfig);
    _cycle = new CycleRunner(*_acq, _trigger, *_ring, _power, _wifi, _tcp,
                             _store, _pin, _battery, _clock);

    _imu.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN);
    _imu.wake(); // once per boot, as main.cpp:114 does — not per acquisition

    // Starts the initial cooldown, which is what keeps the magnet that caused the
    // ext0 wake from ending the first acquisition on its first poll.
    _trigger.begin(millis(), serverConfig.trigger_cooldown_sec);

    // Radio off then 10 MHz. WiFi.config() above enables STA on arduino-esp32, so
    // without this the radio would stay powered right through the acquisition.
    _power.enterAcquisitionMode();
}

void App::run()
{
    BeltInputs in;
    // Always false here, and correctly so: the flag is read and consumed in
    // begin(), before run() is ever called. If it had been set, this boot either
    // never reached run() (it is serving the upload page) or it failed to bring the
    // AP up and left the flag for the next boot. Either way belt_next must not see
    // it, or the device would restart into OTA a second time.
    in.otaFlagSet = false;
    in.acquisitionsDone = _acq->acquisitionsAttempted();
    in.maxAcquisitions = serverConfig.max_acquisitions;
    in.idleTimedOut = _idleTimedOut;
    in.updateRequestedByServer = _updateRequested;

    BeltStep step = belt_next(stage, in);
    stage = step.next;

    switch (step.action)
    {
    case BELT_ACTION_SLEEP_UNTIL_TRIGGER:
        _power.deepSleepUntilTrigger(MAGNET_TRIGGER_PIN);
        break;

    case BELT_ACTION_RUN_CYCLE_ITERATION:
    {
        NetworkConfig network;
        network.ssid = WIFI_SSID;
        network.password = WIFI_PASSWORD;
        network.host = SERVER_HOST;
        network.port = TCP_SERVER_PORT;

        const CycleOutcome outcome = _cycle->runIteration(network, serverConfig);
        _idleTimedOut = outcome.idleTimedOut;
        _updateRequested = outcome.updateRequested;
        break;
    }

    case BELT_ACTION_SLEEP_TIMER:
        _acq->endCycle();
        _power.deepSleepTimer(serverConfig.sleep_time_min);
        break;

    case BELT_ACTION_RESTART:
        // The arming is already persisted — apply_update_field wrote it the moment
        // the response arrived. Restarting here is what enters the OTA boot path,
        // and it MUST happen before another acquisition: the next transmission
        // would carry update=0, which is a disarm, and the device would clear the
        // flag it had just taken. T21b pins that precedence.
        ESP.restart();
        break;

    case BELT_ACTION_ENTER_OTA:
        // Unreachable: the flag is consumed in begin(), before run() is ever
        // called, so belt_next never sees otaFlagSet true. It restarts rather than
        // falling through because no path in this firmware returns without an
        // action — an empty case is what left the refactor spinning.
        ESP.restart();
        break;

    case BELT_ACTION_INVALID:
    default:
        ESP.restart();
        break;
    }
}

void App::runOtaWindow()
{
    Serial.println("Aguardando atualizacao OTA...");

    const uint32_t start = millis();
    const uint32_t timeoutMs = OTA_WINDOW_MINUTES * 60UL * 1000UL;

    while (true)
    {
        _ap.handleClient();
        delay(10);

        if ((millis() - start) >= timeoutMs)
        {
            // The window opened and nobody used it. The flag is already spent, so
            // this restarts into an ordinary cycle. Production does the same at
            // main.cpp:91-95.
            Serial.println("Tempo de espera para OTA esgotado. Reiniciando...");
            ESP.restart();
        }
    }
}
