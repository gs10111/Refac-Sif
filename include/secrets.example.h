#ifndef SECRETS_H
#define SECRETS_H

// TEMPLATE. Copy this file to include/secrets.h and fill in the real values on the
// machine that flashes devices. include/secrets.h is git-ignored and must NEVER be
// committed — this repository is public.
//
//     cp include/secrets.example.h include/secrets.h
//     $EDITOR include/secrets.h
//
// Only the two WiFi credentials live here. The server address and the static IP
// configuration are network topology rather than secrets and stay in
// src/config/network_config.h.
//
// There is deliberately no fallback. A build without include/secrets.h fails with
// an #error rather than compiling a placeholder, because a placeholder that builds
// cleanly produces a device that flashes, boots, samples, and can never reach the
// network — silently, for the rest of its service life.

#define WIFI_SSID     "put-the-real-ssid-here"
#define WIFI_PASSWORD "put-the-real-password-here"

#endif // SECRETS_H
