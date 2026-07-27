#include "cycle_runner.h"

#include "uploader.h"
#include "ota_arming.h"

#define WIFI_CONNECT_TIMEOUT_MS 5000 // main.cpp:191 waits 5 s

CycleRunner::CycleRunner(AcquisitionService &acq,
                         BeltTrigger &trigger,
                         RingBuffer &ring,
                         PowerManager &power,
                         IRadio &radio,
                         ITransport &transport,
                         IKeyValueStore &store,
                         ITriggerSource &pin,
                         IBatterySense &battery,
                         IClock &clock)
    : _acq(acq), _trigger(trigger), _ring(ring), _power(power), _radio(radio),
      _transport(transport), _store(store), _pin(pin), _battery(battery), _clock(clock)
{
}

CycleOutcome CycleRunner::runIteration(const NetworkConfig &network, ServerConfig &config)
{
    CycleOutcome outcome = {false, false};

    _acq.beginAcquisition(_clock.millis());

    AcquisitionResult result;
    do
    {
        const uint32_t now = _clock.millis();
        result = _acq.step(now, _trigger.poll(_pin.read(), now));
    } while (result == ACQ_RUNNING);

    if (result == ACQ_STOPPED_BY_IDLE)
    {
        // The belt stopped. Production discards this acquisition unsent
        // (main.cpp:169 checks before the transmit block) and leaves the ring
        // alone, so what it holds still goes out on the next successful send.
        outcome.idleTimedOut = true;
    }
    else
    {
        _power.enterTransmitMode();

        if (_radio.connect(network.ssid, network.password, WIFI_CONNECT_TIMEOUT_MS))
        {
            // Starts from the config in force, so a truncated or missing response
            // leaves every field exactly as it was.
            ServerConfig received = config;

            const UploadOutcome upload = upload_acquisition(
                _transport, network.host, network.port, _ring, _battery, received);

            if (upload.configReceived)
            {
                config = received;
                _acq.setConfig(config);
                _trigger.setCooldownSec(config.trigger_cooldown_sec);

                // update=1 arms and asks for a restart; update=0 DISARMS a flag left
                // set by a failed AP bring-up. Not silence, as it is in the original
                // (main.cpp:282 has no else).
                outcome.updateRequested = apply_update_field(_store, received.update);
            }

#ifdef MUTANT_RADIO_OFF_ONLY_ON_SUCCESS
            // ==== MUTATION: MUTANT_RADIO_OFF_ONLY_ON_SUCCESS ====
            // Built only by [env:mutant_radio_off_only_on_success].
            // Never by env:native or env:pico32.
            // Run it:  pio test -e mutant_radio_off_only_on_success
            //
            // BREAKS: the radio is turned off only inside the successful-connect
            //         branch — R7 exactly as it shipped. A failed connect, or an
            //         acquisition that timed out and returned early, leaves the
            //         radio powered through the next acquisition at 10 MHz.
            // CAUGHT BY: test_cycle A21 (connect fails) and A23 (idle timeout,
            //         which returns before the transmit block entirely).
            // SURVIVED BY: A20 — on the happy path both versions turn it off, which
            //         is why reading the function and believing it let R7 ship.
            _power.enterAcquisitionMode();
#endif
        }
    }

#ifndef MUTANT_RADIO_OFF_ONLY_ON_SUCCESS
    // The radio goes off whether the connect succeeded, failed, or was never
    // attempted because the acquisition timed out. Production does this on every
    // path through loop() (main.cpp:300-301).
    _power.enterAcquisitionMode();
#endif

    return outcome;
}
