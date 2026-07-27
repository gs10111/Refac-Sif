#ifndef RADIO_H
#define RADIO_H

#include <stdint.h>

// The WiFi radio, as the power path needs to see it.
//
// off() must be unconditional: it is called on paths where the radio was never
// connected, and the refactor's bug was exactly that it only disconnected inside
// the successful-connect branch. Production turns it off on every path through
// loop() (main.cpp:300-301) and again before sleeping (:138).
class IRadio
{
public:
    virtual ~IRadio() {}

    virtual bool connect(const char *ssid, const char *password, uint32_t timeoutMs) = 0;
    virtual void off() = 0;
    virtual bool isConnected() = 0;
};

#endif // RADIO_H
