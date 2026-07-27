#ifndef OTA_ARMING_H
#define OTA_ARMING_H

#include <stdint.h>

#include "key_value_store.h"
#include "access_point.h"

// Namespace "config", key "update" — the same NVS location production uses
// (main.cpp:73-74), so a device flashed over an older build keeps its flag.
#define OTA_FLAG_KEY "update"

enum OtaEntry : uint8_t
{
    OTA_NOT_ARMED = 0,   // ordinary boot; nothing was touched
    OTA_SERVING,         // AP and page are up, the flag is spent, run the window
    OTA_BRINGUP_FAILED   // flag left SET; carry on booting and retry next time
};

// Three outcomes rather than a bool: "not armed" and "tried and failed" lead the
// caller to do different things, and collapsing them would put that decision back
// into Arduino code where nothing can assert it.
OtaEntry enter_ota_if_armed(IAccessPoint &accessPoint,
                            IKeyValueStore &store,
                            const char *ssidPrefix,
                            const char *password);

// Acts on the `update` field of a server response. Returns true when the caller
// must restart into the OTA boot path.
//
// It returns the instruction instead of restarting, so the requirement it carries
// stays visible at the call site: NOTHING MAY TRANSMIT between here and the OTA
// boot. Every ordinary transmission collects update=0, which is now a disarm, so a
// device that finished its cycle first would clear the flag it had just taken.
bool apply_update_field(IKeyValueStore &store, uint16_t updateField);

#endif // OTA_ARMING_H
