#include "wifi_manager.h"
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
        if ((millis() - start) > timeoutMs) return false;
        delay(100);
    }
    return true;
}

void WiFiManager::disconnect()
{
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

bool WiFiManager::isConnected()
{
    return WiFi.status() == WL_CONNECTED;
}
