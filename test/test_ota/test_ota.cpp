// Round 8 — T50..T57 — OTA arming, and the ordering that decides whether an
// operator ever sees an access point.
//
// D3 makes the arming ONE-SHOT: the backend sends update=1 to the next device that
// completes a transmission and clears its own flag in the same lock. So from the
// moment sendall returns, THE DEVICE'S NVS FLAG IS THE ONLY ACTIONABLE RECORD OF
// THE OPERATOR'S INTENT. Everything below follows from that.
//
// Production (main.cpp:77-84) brings the AP up, then clears the flag, then starts
// the web server — and ignores softAP()'s return value. So an AP that fails to come
// up, or one that comes up with no server behind it, still spends the arming: the
// operator sees nothing, or sees a network they cannot use, and the request is gone
// from both halves. That is the phantom AP, reached with every component behaving
// exactly as written.
//
// Clause 1 of the contract adds the other half: update=0 is a DISARM, not silence
// as it is in production (main.cpp:282 has no else).
//
// IT IS A NEW FEATURE, NOT A FIDELITY FIX, and the distinction is load-bearing.
// Production has no latent flag to bound: it always clears on boot. We built both
// the latch — by keeping the flag set when the AP fails to come up — and the bound
// for it. Neither preserves anything the original did.
//
// The check that decides it, so this classification can be tested rather than
// taken: A FIDELITY FIX CAN ALWAYS NAME THE ORIGINAL BEHAVIOUR IT RESTORES, WITH A
// LINE NUMBER. A justification that argues why the change is NECESSARY is a
// feature — necessity is not fidelity.
//
// The tell is visible in the citation itself. Look at the line above: it cites
// main.cpp:282 to say there is NO ELSE. A fidelity fix cites what production DOES;
// a feature cites what production does NOT do and argues from there. That is what
// makes this mechanical rather than a judgement call, and it is what nobody ran
// before this clause was first signed as a fidelity fix.
//
// (The RULE is process and its canonical home is docs/qa/test-plan.md section 0.
// What is stated here is its APPLICATION to this clause — a fact about this
// citation, which cannot drift from the rule because it is not a copy of it. If the
// two ever disagree, section 0 wins.)
//
// The trade, stated so a reader can weigh it rather than accept it: without the
// bound, a device whose bring-up keeps failing holds the flag indefinitely and can
// walk into AP mode at an arbitrary future wake — weeks later, no operator present,
// the five-minute window opening and closing to an empty plant, the sensor off the
// network until it times out. With it, the request is discarded promptly and
// visibly while the operator is still at the desk. Prompt and visible beats latent.
//
// Read the disarm honestly: it does not honour the operator's request, it discards
// it cleanly. Lost better, not honoured.
//
// CLAUSE 2 EXISTS ONLY BECAUSE CLAUSE 1 DOES. Nothing may transmit between
// receiving update=1 and the OTA boot, because that transmission would carry
// update=0 and clear the flag it had just taken. Production cannot have this
// requirement — its update=0 is inert. T21b in test_belt_cycle defends it.
// Reversing clause 1 removes clause 2 in the same motion: one decision, not two.

#include <unity.h>
#include <stdint.h>
#include <string.h>

#include "packet.h"
#include "key_value_store.h"
#include "access_point.h"
#include "ota_arming.h"

void setUp(void) {}
void tearDown(void) {}

// --- shared call log ------------------------------------------------------
static const uint32_t kMaxCalls = 16;

class CallLog
{
public:
    CallLog() : _count(0) {}
    void record(const char *tag)
    {
        if (_count < kMaxCalls)
            _tags[_count++] = tag;
    }
    uint32_t count() const { return _count; }
    const char *at(uint32_t i) const { return i < _count ? _tags[i] : "<past end>"; }
    bool contains(const char *tag) const
    {
        for (uint32_t i = 0; i < _count; i++)
            if (strcmp(_tags[i], tag) == 0)
                return true;
        return false;
    }

private:
    const char *_tags[kMaxCalls];
    uint32_t _count;
};

// --- fakes ----------------------------------------------------------------
class FakeStore : public IKeyValueStore
{
public:
    FakeStore(CallLog &log, bool armed) : _log(log), _value(armed), _writes(0) {}

    bool getBool(const char *, bool) override { return _value; }

    void putBool(const char *, bool value) override
    {
        _writes++;
        _value = value;
        _log.record(value ? "store.set" : "store.clear");
    }

    bool value() const { return _value; }
    uint32_t writes() const { return _writes; }

private:
    CallLog &_log;
    bool _value;
    uint32_t _writes;
    // Not exercised here: OTA arming is a bool. Present so the fake still
    // satisfies the interface the rate field added.
    uint16_t getUShort(const char *, uint16_t defaultValue) override { return defaultValue; }
    void putUShort(const char *, uint16_t) override {}
};

class FakeAccessPoint : public IAccessPoint
{
public:
    FakeAccessPoint(CallLog &log, bool startOk, bool serveOk)
        : _log(log), _startOk(startOk), _serveOk(serveOk) {}

    bool start(const char *, const char *) override
    {
        _log.record("ap.start");
        return _startOk;
    }

    bool serveUpdatePage() override
    {
        _log.record("ap.serve");
        return _serveOk;
    }

    void stop() override { _log.record("ap.stop"); }

private:
    CallLog &_log;
    bool _startOk;
    bool _serveOk;
};

// T50 — the ordering. The flag is spent only once the operator can actually reach
// the page: AP up, then page served, then and only then cleared.
static void test_ota_flag_is_cleared_only_after_the_access_point_and_the_page_are_both_up(void)
{
    CallLog log;
    FakeStore store(log, true);
    FakeAccessPoint ap(log, true, true);

    OtaEntry entry = enter_ota_if_armed(ap, store, "ssid", "password");

    TEST_ASSERT_EQUAL_UINT8(OTA_SERVING, entry);
    TEST_ASSERT_EQUAL_UINT32(3, log.count());
    TEST_ASSERT_EQUAL_STRING("ap.start", log.at(0));
    TEST_ASSERT_EQUAL_STRING("ap.serve", log.at(1));
    TEST_ASSERT_EQUAL_STRING("store.clear", log.at(2));
    TEST_ASSERT_FALSE(store.value());
}

// T51 — softAP failed. Production discards the arming here; we keep it, because the
// operator's request has nowhere else to live. The page is never even attempted.
static void test_ota_flag_survives_a_failed_access_point_so_the_next_boot_retries(void)
{
    CallLog log;
    FakeStore store(log, true);
    FakeAccessPoint ap(log, false, true);

    OtaEntry entry = enter_ota_if_armed(ap, store, "ssid", "password");

    TEST_ASSERT_EQUAL_UINT8(OTA_BRINGUP_FAILED, entry);
    TEST_ASSERT_TRUE(store.value());
    TEST_ASSERT_EQUAL_UINT32(0, store.writes());
    TEST_ASSERT_FALSE(log.contains("ap.serve"));
}

// T52 — the AP came up but the page did not. This is the one production cannot
// even see: it cleared the flag before starting the server, so the operator gets a
// network they can join and a page that does not exist, and the arming is gone.
static void test_ota_flag_survives_a_failed_page_start_so_the_next_boot_retries(void)
{
    CallLog log;
    FakeStore store(log, true);
    FakeAccessPoint ap(log, true, false);

    OtaEntry entry = enter_ota_if_armed(ap, store, "ssid", "password");

    TEST_ASSERT_EQUAL_UINT8(OTA_BRINGUP_FAILED, entry);
    TEST_ASSERT_TRUE(store.value());
    TEST_ASSERT_EQUAL_UINT32(0, store.writes());
}

// T54 — the ordinary boot. Nothing is touched, and the radio is never put into AP
// mode, which matters because this path runs on every wake of every device.
static void test_ota_is_not_entered_when_the_flag_is_clear(void)
{
    CallLog log;
    FakeStore store(log, false);
    FakeAccessPoint ap(log, true, true);

    OtaEntry entry = enter_ota_if_armed(ap, store, "ssid", "password");

    TEST_ASSERT_EQUAL_UINT8(OTA_NOT_ARMED, entry);
    TEST_ASSERT_EQUAL_UINT32(0, log.count());
    TEST_ASSERT_EQUAL_UINT32(0, store.writes());
}

// T57 — update=1 persists the arming and tells the caller to restart. Nothing may
// transmit between here and the OTA boot path — see T21b in test_belt_cycle, which
// pins the precedence that guarantees it.
static void test_update_one_persists_the_arming_and_requests_a_restart(void)
{
    CallLog log;
    FakeStore store(log, false);

    const bool restartNeeded = apply_update_field(store, 1);

    TEST_ASSERT_TRUE(restartNeeded);
    TEST_ASSERT_TRUE(store.value());
    TEST_ASSERT_EQUAL_UINT32(1, store.writes());
}

// T55 — clause 1. update=0 is an instruction, not silence. Every ordinary
// transmission carries it, so a flag left set by a failed bring-up is cleared by
// the device's own next upload rather than latching until some future wake when the
// interference has cleared and nobody is expecting an access point.
static void test_update_zero_clears_a_set_ota_flag(void)
{
    CallLog log;
    FakeStore store(log, true);

    const bool restartNeeded = apply_update_field(store, 0);

    TEST_ASSERT_FALSE(restartNeeded);
    TEST_ASSERT_FALSE(store.value());
    TEST_ASSERT_EQUAL_UINT32(1, store.writes());
}

// T56 — read before write. update=0 arrives on every non-armed connection for the
// life of the device; writing on each one would spend flash endurance to store a
// value that is already there.
static void test_update_zero_does_not_write_when_the_flag_is_already_clear(void)
{
    CallLog log;
    FakeStore store(log, false);

    const bool restartNeeded = apply_update_field(store, 0);

    TEST_ASSERT_FALSE(restartNeeded);
    TEST_ASSERT_FALSE(store.value());
    TEST_ASSERT_EQUAL_UINT32(0, store.writes());
}

static int run_all(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ota_flag_is_cleared_only_after_the_access_point_and_the_page_are_both_up);
    RUN_TEST(test_ota_flag_survives_a_failed_access_point_so_the_next_boot_retries);
    RUN_TEST(test_ota_flag_survives_a_failed_page_start_so_the_next_boot_retries);
    RUN_TEST(test_ota_is_not_entered_when_the_flag_is_clear);
    RUN_TEST(test_update_one_persists_the_arming_and_requests_a_restart);
    RUN_TEST(test_update_zero_clears_a_set_ota_flag);
    RUN_TEST(test_update_zero_does_not_write_when_the_flag_is_already_clear);
    return UNITY_END();
}

#ifdef ARDUINO
#include <Arduino.h>
void setup()
{
    delay(2000);
    run_all();
}
void loop() {}
#else
int main(void)
{
    return run_all();
}
#endif
