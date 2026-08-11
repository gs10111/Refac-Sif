#ifndef KEY_VALUE_STORE_H
#define KEY_VALUE_STORE_H

#include <stdint.h>

// Persistent key/value storage that survives a restart.
//
// The OTA flag lives here rather than in RTC memory precisely because it has to
// survive the ESP.restart() that arms it, and RTC memory does not — it is reloaded
// from flash on every reset except a deep-sleep wake.
class IKeyValueStore
{
public:
    virtual ~IKeyValueStore() {}

    virtual bool getBool(const char *key, bool defaultValue) = 0;
    virtual void putBool(const char *key, bool value) = 0;

    // The acquisition rate lives here for the same reason: it has to survive the
    // deep sleep AND the restart, and it is what the device runs before the
    // server has had a chance to say anything on this boot.
    virtual uint16_t getUShort(const char *key, uint16_t defaultValue) = 0;
    virtual void putUShort(const char *key, uint16_t value) = 0;
};

#endif // KEY_VALUE_STORE_H
