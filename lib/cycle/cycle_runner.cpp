#include "cycle_runner.h"

#include "uploader.h"
#include "ota_arming.h"
#include "log.h"

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

    SIF_LOG("Coletando dados"); // main.cpp:166, verbatim
    SIF_TIMER(acquisitionStart);

    // Measured with the injected clock, not with the log macros: SIF_TIMER
    // compiles to nothing off-target, and this number now leaves the device.
    const uint32_t acquisitionStartMs = _clock.millis();
    _acq.beginAcquisition(acquisitionStartMs);

    AcquisitionResult result;
    uint32_t lastNow = acquisitionStartMs;
    do
    {
        const uint32_t now = _clock.millis();
        lastNow = now;
        result = _acq.step(now, _trigger.poll(_pin.read(), now));
    } while (result == ACQ_RUNNING);

    // What the sensor ACHIEVED, not what it was told to run: frames actually
    // taken over the span they were taken in. Counted by the service, NOT read
    // from the ring's occupancy — a capture that outruns the buffer would then
    // report a rate as low as the buffer is small (200 Hz for ten minutes reads
    // as 100).
    const uint16_t achievedHz = effective_hz(
        _acq.framesStored(), lastNow - acquisitionStartMs);

    SIF_LOG(SIF_ELAPSED(acquisitionStart)); // main.cpp:180, elapsed ms

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
                _transport, network.host, network.port, _ring, _battery,
                achievedHz, received);

            if (upload.configReceived)
            {
                // The rate is the one field the device must remember across the
                // deep sleep, so it is decided here rather than carried in RAM.
                // A code the part cannot run — 0 from a server with no opinion,
                // or a nibble the datasheet Reserves — leaves both NVS and the
                // running config on the rate already in force. Writing only on
                // a real change keeps the flash out of the per-capture path.
#ifdef MUTANT_SAMPLING_TRUSTS_SERVER
                // ==== MUTATION: MUTANT_SAMPLING_TRUSTS_SERVER ====
                // Built only by [env:mutant_sampling_trusts_server].
                // Never by env:native or env:pico32.
                // Run it:  pio test -e mutant_sampling_trusts_server
                //
                // BREAKS: whatever the server sends is stored as the rate, with no
                //         whitelist. A nibble the datasheet Reserves (12-14), or the
                //         0 a server with no opinion sends, is persisted and applied
                //         at the next boot.
                // WHY:    the device still runs and still transmits, so nothing fails
                //         loudly. It samples in a mode the part does not define.
                // CAUGHT BY: test_cycle test_a_reserved_nibble_from_the_server_is_refused
                //            and test_a_server_with_no_opinion_leaves_the_stored_rate_alone.
                if (received.sampling_code !=
                    _store.getUShort(SAMPLING_CODE_NVS_KEY, DEFAULT_SAMPLING_CODE))
                {
                    _store.putUShort(SAMPLING_CODE_NVS_KEY, received.sampling_code);
                }
#elif defined(MUTANT_SAMPLING_WRITES_UNCONDITIONALLY)
                // ==== MUTATION: MUTANT_SAMPLING_WRITES_UNCONDITIONALLY ====
                // Built only by [env:mutant_sampling_writes_unconditionally].
                // Never by env:native or env:pico32.
                // Run it:  pio test -e mutant_sampling_writes_unconditionally
                //
                // BREAKS: the rate is written to NVS on every config receipt rather
                //         than only when it changes. Right state, wrong write count.
                // WHY:    invisible to any test that only checks which rate ended up
                //         stored; its cost is flash endurance, one write per capture.
                // CAUGHT BY: test_cycle test_the_rate_already_stored_is_not_written_again.
                if (!is_valid_sampling_code(received.sampling_code))
                {
                    received.sampling_code = config.sampling_code;
                }
                else
                {
                    _store.putUShort(SAMPLING_CODE_NVS_KEY, received.sampling_code);
                }
#else
                if (!is_valid_sampling_code(received.sampling_code))
                {
                    SIF_LOGF("Taxa ignorada: codigo %u nao e valido; mantida %u.\n",
                             (unsigned)received.sampling_code,
                             (unsigned)config.sampling_code);
                    received.sampling_code = config.sampling_code;
                }
                else if (received.sampling_code !=
                         _store.getUShort(SAMPLING_CODE_NVS_KEY, DEFAULT_SAMPLING_CODE))
                {
                    _store.putUShort(SAMPLING_CODE_NVS_KEY, received.sampling_code);
                    // Written to NVS so it survives the deep sleep, and applied by
                    // AcquisitionService on the very next acquisition — the
                    // operator does not wait for a boot to see the change.
                    SIF_LOGF("Nova taxa gravada: codigo ODR %u. "
                             "Vale a partir da proxima aquisicao.\n",
                             (unsigned)received.sampling_code);
                }
                else
                {
                    SIF_LOGF("Taxa mantida: codigo ODR %u.\n",
                             (unsigned)received.sampling_code);
                }
#endif

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
