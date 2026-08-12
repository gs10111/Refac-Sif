// Why a WiFi connection attempt gave up, in words the person at the bench can act on.
//
// The device printed fifty dots and stopped. Fifty dots is the five-second
// timeout, and it says nothing about WHY: a wrong password, an SSID that is only
// on 5 GHz, and an access point that is simply out of range all look identical.
// The status the radio already holds separates them, and it was being thrown away.
//
// The mapping is here, off-target, so the words are pinned by a test rather than
// by whoever is holding the board.

#include <unity.h>
#include <string.h>

#include "wifi_status.h"

void setUp(void) {}
void tearDown(void) {}

static void test_no_ssid_says_the_network_was_not_found(void)
{
    const char *text = wifi_status_text(SIF_WIFI_NO_SSID_AVAIL);

    // The 2.4 GHz hint is the point: this is what an ESP32 reports for a network
    // that exists but only on 5 GHz, which no ESP32 can see.
    TEST_ASSERT_NOT_NULL(strstr(text, "nao encontrada"));
    TEST_ASSERT_NOT_NULL(strstr(text, "2,4 GHz"));
}

static void test_connect_failed_points_at_the_password(void)
{
    TEST_ASSERT_NOT_NULL(strstr(wifi_status_text(SIF_WIFI_CONNECT_FAILED), "senha"));
}

static void test_disconnected_and_idle_are_told_apart(void)
{
    const char *disconnected = wifi_status_text(SIF_WIFI_DISCONNECTED);
    const char *idle = wifi_status_text(SIF_WIFI_IDLE);

    TEST_ASSERT_NOT_NULL(disconnected);
    TEST_ASSERT_NOT_NULL(idle);
    TEST_ASSERT_TRUE(strcmp(disconnected, idle) != 0);
}

static void test_connected_is_described_too(void)
{
    // Reached when the radio connects in the same millisecond the timeout expires.
    // A "connected" that prints as an unknown code would send someone hunting a
    // radio fault that is not there.
    TEST_ASSERT_NOT_NULL(strstr(wifi_status_text(SIF_WIFI_CONNECTED), "conectado"));
}

static void test_a_code_nobody_mapped_is_still_readable(void)
{
    // Arduino cores add status values. Falling back to "unknown" without saying
    // WHICH unknown would leave the next person with less than the raw number.
    const char *text = wifi_status_text(99);

    TEST_ASSERT_NOT_NULL(strstr(text, "desconhecido"));
}

static void test_no_message_is_empty(void)
{
    // An empty string in the log reads as "the print itself is broken".
    for (int status = -1; status <= 8; status++)
    {
        const char *text = wifi_status_text(status);
        TEST_ASSERT_NOT_NULL(text);
        TEST_ASSERT_TRUE(strlen(text) > 0);
    }
}

static void test_the_password_is_never_part_of_the_message(void)
{
    // The serial console is read over the shoulder and pasted into chats. The
    // status codes carry no credentials, and nothing here may add any.
    for (int status = -1; status <= 8; status++)
    {
        TEST_ASSERT_NULL(strstr(wifi_status_text(status), "WIFI_PASSWORD"));
    }
}

static int run_all(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_no_ssid_says_the_network_was_not_found);
    RUN_TEST(test_connect_failed_points_at_the_password);
    RUN_TEST(test_disconnected_and_idle_are_told_apart);
    RUN_TEST(test_connected_is_described_too);
    RUN_TEST(test_a_code_nobody_mapped_is_still_readable);
    RUN_TEST(test_no_message_is_empty);
    RUN_TEST(test_the_password_is_never_part_of_the_message);
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
