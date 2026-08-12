#ifndef WIFI_STATUS_H
#define WIFI_STATUS_H

// Why a WiFi connection attempt gave up, in words.
//
// The connect loop prints a dot every 100 ms and returns false after the timeout.
// Fifty dots is all the bench ever saw, and a wrong password, an SSID that only
// exists on 5 GHz and an access point out of range produce exactly the same fifty
// dots. The radio already holds a status that separates them; this turns it into
// a sentence.
//
// The numbers are wl_status_t from the Arduino core, restated here so the mapping
// builds and is tested off-target. wifi_manager.cpp static_asserts them against
// the real enum, so a core that renumbers breaks the build instead of printing
// the wrong reason.

#define SIF_WIFI_IDLE            0
#define SIF_WIFI_NO_SSID_AVAIL   1
#define SIF_WIFI_SCAN_COMPLETED  2
#define SIF_WIFI_CONNECTED       3
#define SIF_WIFI_CONNECT_FAILED  4
#define SIF_WIFI_CONNECTION_LOST 5
#define SIF_WIFI_DISCONNECTED    6

// Never null, never empty, and never carries a credential: the serial console is
// read over the shoulder and pasted into chats.
const char *wifi_status_text(int status);

#endif // WIFI_STATUS_H
