#ifndef CYCLE_RUNNER_H
#define CYCLE_RUNNER_H

#include <stdint.h>

#include "packet.h"
#include "ring_buffer.h"
#include "belt_trigger.h"
#include "acquisition.h"
#include "power_manager.h"
#include "radio.h"
#include "transport.h"
#include "key_value_store.h"
#include "trigger_source.h"
#include "battery_sense.h"
#include "clock.h"

struct NetworkConfig
{
    const char *ssid;
    const char *password;
    const char *host;
    uint16_t port;
};

struct CycleOutcome
{
    bool idleTimedOut;
    bool updateRequested;
};

// There is deliberately no `transmitted` field. A failed upload is not acted on —
// production breaks out of its send loop and carries straight on (main.cpp:248-251)
// — so a caller has nothing to do with the answer, and a field nobody reads invites
// the next reader to believe the failure is handled somewhere.

// One acquisition and its upload — the loop that used to live in App and could not
// be reached from a host build.
//
// Ten collaborators, which is the honest count for a wiring point: every one of
// them is something this loop genuinely drives. Credentials arrive as a parameter
// rather than as macros, so nothing here has ever heard of WIFI_SSID.
class CycleRunner
{
public:
    CycleRunner(AcquisitionService &acq,
                BeltTrigger &trigger,
                RingBuffer &ring,
                PowerManager &power,
                IRadio &radio,
                ITransport &transport,
                IKeyValueStore &store,
                ITriggerSource &pin,
                IBatterySense &battery,
                IClock &clock);

    // Ends with the radio off on EVERY path — the guarantee that was structural
    // from round 6 until this class existed, and whose absence is R7.
    CycleOutcome runIteration(const NetworkConfig &network, ServerConfig &config);

private:
    AcquisitionService &_acq;
    BeltTrigger &_trigger;
    RingBuffer &_ring;
    PowerManager &_power;
    IRadio &_radio;
    ITransport &_transport;
    IKeyValueStore &_store;
    ITriggerSource &_pin;
    IBatterySense &_battery;
    IClock &_clock;
};

#endif // CYCLE_RUNNER_H
