#include "ota_arming.h"

OtaEntry enter_ota_if_armed(IAccessPoint &accessPoint,
                            IKeyValueStore &store,
                            const char *ssidPrefix,
                            const char *password)
{
    if (!store.getBool(OTA_FLAG_KEY, false))
        return OTA_NOT_ARMED;

#ifdef MUTANT_CLEAR_OTA_FLAG_BEFORE_AP
    // ==== MUTATION: MUTANT_CLEAR_OTA_FLAG_BEFORE_AP ====
    // Built only by [env:mutant_clear_ota_flag_before_ap].
    // Never by env:native or env:pico32.
    // Run it:  pio test -e mutant_clear_ota_flag_before_ap
    //
    // BREAKS: the flag is spent before the access point exists.
    //
    // NOT A PRODUCTION DEFECT WE FIXED. This IS production's order (main.cpp:78
    // then :81, page later still), and the happy path is byte-identical either
    // way — flag cleared, page served. The orders diverge only when softAP()
    // succeeds and the page does not, which is DEC-0 clause (d): a latent bug on a
    // path the reachable one never touches, where the correct implementation is
    // preferred. It is a clause (d) preference, not a listed regression, and three
    // tests killing this mutation should not be read as production having been
    // broken.
    // CAUGHT BY: test_ota T50 (on the call sequence), T51 and T52 (on the flag
    //         surviving a failure).
    // SURVIVED BY: T54, T55, T56, T57 — none of them reach this function with the
    //         flag set.
    store.putBool(OTA_FLAG_KEY, false);

    if (!accessPoint.start(ssidPrefix, password))
        return OTA_BRINGUP_FAILED;

    if (!accessPoint.serveUpdatePage())
        return OTA_BRINGUP_FAILED;

    return OTA_SERVING;
#else
    // The operator's request is the only record left once the backend has spent its
    // arming, so it is not discarded until the page is genuinely reachable.
    //
    // Production clears between softAP() and configureOtaServer() (main.cpp:78,
    // :81, :84) and ignores softAP()'s return. That is not a defect on any path the
    // shipped firmware reaches — the two orders agree whenever both steps succeed.
    // This is the DEC-0 clause (d) preference: correct on the latent path, identical
    // on the reachable one.
    if (!accessPoint.start(ssidPrefix, password))
        return OTA_BRINGUP_FAILED;

    if (!accessPoint.serveUpdatePage())
        return OTA_BRINGUP_FAILED;

    store.putBool(OTA_FLAG_KEY, false);
    return OTA_SERVING;
#endif
}

bool apply_update_field(IKeyValueStore &store, uint16_t updateField)
{
    if (updateField != 0)
    {
        // No read-before-write here, and the asymmetry is deliberate: update=1 is
        // rare by construction — one connection per operator action — while
        // update=0 arrives on every transmission for the life of the device. The
        // guard belongs where the traffic is.
        store.putBool(OTA_FLAG_KEY, true);
        return true;
    }

#ifdef MUTANT_DISARM_WRITES_UNCONDITIONALLY
    // ==== MUTATION: MUTANT_DISARM_WRITES_UNCONDITIONALLY ====
    // Built only by [env:mutant_disarm_writes_unconditionally].
    // Never by env:native or env:pico32.
    // Run it:  pio test -e mutant_disarm_writes_unconditionally
    //
    // BREAKS: the disarm writes on every config receipt instead of only on the
    //         transition. Right state, wrong write count — one NVS write per
    //         connection, forever.
    // WHY:    it is invisible to every test that checks state, because the flag
    //         ends up correct either way. Its cost is flash endurance.
    // CAUGHT BY: test_ota T56.
    // SURVIVED BY: T55 and T57 — both end with the flag in the right state and the
    //         expected number of writes.
    store.putBool(OTA_FLAG_KEY, false);
#else
    if (store.getBool(OTA_FLAG_KEY, false))
        store.putBool(OTA_FLAG_KEY, false);
#endif

    return false;
}
