#ifndef ESP32_PLATFORM_H
#define ESP32_PLATFORM_H

#include "Arduino.h"
#include "esp_sleep.h"
#include "board.h"
#include "allocator.h"
#include "fault.h"
#include "cpu.h"
#include "sleeper.h"
#include "key_value_store.h"
#include "access_point.h"
#include <Preferences.h>
#include <WebServer.h>
#include <Update.h>
#include <WiFi.h>
#include "../webpage.h"

// The acquisition buffer lives in PSRAM — 700000 bytes will not fit in internal
// RAM.
class PsramAllocator : public IAllocator
{
public:
    uint8_t *allocate(uint32_t bytes) override
    {
        return (uint8_t *)ps_malloc(bytes);
    }
};

// Matches production exactly: print, then spin forever (main.cpp:61-62). It does
// not return and it deliberately does not reboot — a device that reboot-loops on a
// hardware fault is harder to diagnose in a plant than one sitting dead with a
// message on the serial line, and production chose the latter.
class HaltFault : public IFault
{
public:
    void fatal(const char *message) override
    {
        Serial.println(message);
        Serial.flush();
        while (true)
        {
        }
    }
};


class EspCpu : public ICpu
{
public:
    void setFrequencyMhz(uint32_t mhz) override { setCpuFrequencyMhz(mhz); }
    void delayMs(uint32_t ms) override { delay(ms); }
};

class EspSleeper : public ISleeper
{
public:
    void enableTimerWakeup(uint64_t us) override
    {
        esp_sleep_enable_timer_wakeup(us);
    }

    void enableExt0Wakeup(uint8_t pin, uint8_t level) override
    {
        esp_sleep_enable_ext0_wakeup((gpio_num_t)pin, level);
    }

    void start() override
    {
        // Production flushes before every deep sleep (main.cpp:144 and :152). A
        // message still sitting in the TX buffer when the CPU stops is a message
        // nobody ever sees.
        Serial.flush();
        esp_deep_sleep_start();
    }
};

// Battery sense on the ADC pin. Same arithmetic as production's
// map(raw, 0, 4095, 0, 19803) at main.cpp:259-260, which is an integer map with a
// zero origin, so the multiply-then-divide is equivalent. 4095 * 19803 fits a
// uint32 with room to spare.
inline uint16_t esp32_read_battery_mv()
{
    uint32_t raw = analogRead(BATTERY_ADC_PIN);
    return (uint16_t)((raw * BATTERY_MV_MAX) / BATTERY_ADC_MAX);
}


// NVS, namespace "config" — the same location production uses (main.cpp:73), so a
// device flashed over an older build keeps whatever flag it was holding.
class NvsStore : public IKeyValueStore
{
public:
    bool getBool(const char *key, bool defaultValue) override
    {
        Preferences prefs;
        prefs.begin("config", true);
        const bool value = prefs.getBool(key, defaultValue);
        prefs.end();
        return value;
    }

    void putBool(const char *key, bool value) override
    {
        Preferences prefs;
        prefs.begin("config", false);
        prefs.putBool(key, value);
        prefs.end();
    }
};

// SoftAP plus the upload page, matching production's configureOtaServer
// (main.cpp:347-405): "/" serves the page, "/update" takes the firmware, "/version"
// and "/restartESP" as before.
class EspAccessPoint : public IAccessPoint
{
public:
    EspAccessPoint() : _server(80) {}

    bool start(const char *ssidPrefix, const char *password) override
    {
        WiFi.mode(WIFI_AP);
        // "Update driver - <MAC>", exactly as main.cpp:78 builds it.
        return WiFi.softAP(String(ssidPrefix) + String(WiFi.macAddress()), password);
    }

    // NOTE: arduino-esp32's WebServer::begin() returns void, so there is nothing to
    // check and this reports success unconditionally. The bool stays in the
    // interface because the CONTRACT needs it — the flag must not be spent until
    // the page is reachable — and if a future core exposes a status, only this
    // adapter changes. T52 therefore defends the ordering rather than a failure the
    // current hardware can produce; T51's failure IS reachable, because softAP()
    // does return a bool.
    bool serveUpdatePage() override
    {
        _server.on("/", HTTP_GET, [this]()
                   { _server.send_P(200, "text/html", updatePage); });

        _server.on(
            "/update", HTTP_POST,
            [this]()
            {
                _server.sendHeader("Connection", "close");
                if (Update.hasError())
                    _server.send(202, "text/plain", "Falha na atualizacao");
                else
                    _server.send(200, "text/plain", "Atualizacao concluida, reiniciando...");
            },
            [this]()
            {
                HTTPUpload &upload = _server.upload();
                if (upload.status == UPLOAD_FILE_START)
                {
                    if (!Update.begin(UPDATE_SIZE_UNKNOWN))
                        Update.printError(Serial);
                }
                else if (upload.status == UPLOAD_FILE_WRITE)
                {
                    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
                        Update.printError(Serial);
                }
                else if (upload.status == UPLOAD_FILE_END)
                {
                    if (Update.end(true))
                    {
                        Serial.println("OTA concluida. Reiniciando...");
                        delay(100);
                        ESP.restart();
                    }
                    else
                    {
                        Update.printError(Serial);
                    }
                }
            });

        _server.on("/version", HTTP_GET, [this]()
                   { _server.send(200, "text/plain", "Versao 1.1"); });

        _server.on("/restartESP", HTTP_POST, [this]()
                   { _server.send(200, "text/plain", "Reiniciando ESP..."); ESP.restart(); });

        _server.begin();
        return true;
    }

    void stop() override
    {
        _server.stop();
        WiFi.softAPdisconnect(true);
    }

    void handleClient() { _server.handleClient(); }

private:
    WebServer _server;
};

#endif // ESP32_PLATFORM_H
