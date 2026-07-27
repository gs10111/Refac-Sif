#ifndef NETWORK_CONFIG_H
#define NETWORK_CONFIG_H

#include <IPAddress.h>

// WiFi credentials come from include/secrets.h, which is git-ignored. See
// include/secrets.example.h.
//
// The guard below is the whole point of the arrangement: a missing secrets.h must
// break the build loudly. The alternative — a committed placeholder — compiles,
// links, flashes and boots, and produces a device that acquires perfectly and can
// never reach the server. That failure is invisible until someone walks out to the
// conveyor and finds a sensor that has never uploaded anything.
#if __has_include("secrets.h")
#include "secrets.h"
#endif

#if !defined(WIFI_SSID) || !defined(WIFI_PASSWORD)
#error "Missing include/secrets.h - copy include/secrets.example.h and fill in the real credentials. Never commit it."
#endif

// Not secrets: network topology, identical across every device on the belt.
#define SERVER_HOST "192.168.1.100"

#define DEVICE_IP      IPAddress(192, 168, 1, 118)
#define NETWORK_GW     IPAddress(192, 168, 1, 1)
#define NETWORK_SUBNET IPAddress(255, 255, 255, 0)

#endif // NETWORK_CONFIG_H
