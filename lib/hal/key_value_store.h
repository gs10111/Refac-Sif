#ifndef KEY_VALUE_STORE_H
#define KEY_VALUE_STORE_H

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
};

#endif // KEY_VALUE_STORE_H
