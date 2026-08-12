#include "wifi_manager.h"

#include "wifi_status.h"
#include "WiFi.h"
#include "esp_wifi.h"

void WiFiManager::configure(IPAddress ip, IPAddress gateway, IPAddress subnet)
{
    WiFi.config(ip, gateway, subnet);
}

bool WiFiManager::connect(const char *ssid, const char *password, uint32_t timeoutMs)
{
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    esp_wifi_config_80211_tx_rate(WIFI_IF_STA, WIFI_PHY_RATE_MCS7_LGI);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if ((millis() - start) > timeoutMs) {
            // The dots alone say only that it gave up. A wrong password, an SSID
            // that lives on 5 GHz and an AP out of range produce exactly the same
            // fifty dots, and this is the one moment the radio still knows which
            // of them happened.
            const int status = (int)WiFi.status();
            Serial.printf("\nWi-Fi nao conectou em %u ms: %s (status %d)\n",
                          (unsigned)timeoutMs, wifi_status_text(status), status);
            return false;
        }
        Serial.print("."); // main.cpp:193 — the pontinhos, asked for by name
        delay(100);
    }

    Serial.print("\n");                    // main.cpp:198
    Serial.println("Conectado ao Wi-Fi.");  // main.cpp:199
    // The radio is pinned to MCS7 (esp_wifi_config_80211_tx_rate above, as
    // production does at main.cpp:216), so a weak link loses data frames while
    // the handshake still gets through. Below roughly -75 dBm that is the first
    // thing to suspect when bytes leave the device and never arrive.
    Serial.printf("RSSI %d dBm, IP %s\n", WiFi.RSSI(), WiFi.localIP().toString().c_str());
    Serial.println("Tempo de Conexão");     // main.cpp:200
    Serial.println(millis() - start);       // main.cpp:201
    return true;
}

void WiFiManager::off()
{
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

bool WiFiManager::isConnected()
{
    return WiFi.status() == WL_CONNECTED;
}

// The numbers in wifi_status.h are wl_status_t restated so the mapping can be
// tested without Arduino. Pinned here: a core that renumbers breaks the build
// instead of printing the wrong reason at the bench.
static_assert((int)WL_IDLE_STATUS == SIF_WIFI_IDLE, "wl_status_t renumbered");
static_assert((int)WL_NO_SSID_AVAIL == SIF_WIFI_NO_SSID_AVAIL, "wl_status_t renumbered");
static_assert((int)WL_SCAN_COMPLETED == SIF_WIFI_SCAN_COMPLETED, "wl_status_t renumbered");
static_assert((int)WL_CONNECTED == SIF_WIFI_CONNECTED, "wl_status_t renumbered");
static_assert((int)WL_CONNECT_FAILED == SIF_WIFI_CONNECT_FAILED, "wl_status_t renumbered");
static_assert((int)WL_CONNECTION_LOST == SIF_WIFI_CONNECTION_LOST, "wl_status_t renumbered");
static_assert((int)WL_DISCONNECTED == SIF_WIFI_DISCONNECTED, "wl_status_t renumbered");
