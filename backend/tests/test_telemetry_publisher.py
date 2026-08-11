"""Every capture reaches ThingsBoard with the instant each sample was taken.

Acceptance criterion: the bursts the sensors send are published to a local MQTT
broker in the shape the thingsboard-gateway forwards, with the raw samples and
the real time of each magnetic event preserved — not the arrival time of the
message, which would collapse a whole capture onto one instant.

No network here: the MQTT client is injected. The publisher never imports paho
unless it is asked to build its own client, so this file runs on a machine that
has no broker and no paho installed.
"""
import datetime

import pytest

from telemetry.publisher import (
    MAX_MEAN_INTERVAL_MS, TELEMETRY_FIELDS, TelemetryPublisher,
    anchor_timestamps, build_burst_summary, build_telemetry_payload, chunked,
    is_plausible_burst,
)

# A queue row: [timestamp_ms, x_data, x_gyro, y_data, y_gyro, z_data, z_gyro, temp]
def row(ts, first=1):
    return [ts, first, 2, 3, 4, 5, 6, 7]


ANCHOR = datetime.datetime(2026, 8, 11, 18, 30, 0)
ANCHOR_MS = int(ANCHOR.timestamp() * 1000)


class FakeMqttClient:
    """Records what would have been published, and answers with a chosen rc."""

    def __init__(self, rc=0):
        self.published = []
        self._rc = rc

    def publish(self, topic, payload, qos=0):
        self.published.append((topic, payload, qos))
        return type('Result', (), {'rc': self._rc})()


# --------------------------------------------------------------------------
# Anchoring — millis() since boot becomes absolute time
# --------------------------------------------------------------------------

def test_the_last_sample_lands_on_the_anchor():
    """The ESP32 restarts millis() on every deep sleep, so a capture arrives
    starting near zero. The anchor is the instant the connection closed."""
    assert anchor_timestamps([10, 30, 50], 1_000_000)[-1] == 1_000_000


def test_earlier_samples_keep_the_spacing_the_sensor_measured():
    """The gaps are the sensor's own; only the origin moves."""
    absolute = anchor_timestamps([10, 30, 50], 1_000_000)

    assert absolute == [999_960, 999_980, 1_000_000]


def test_anchoring_nothing_yields_nothing():
    assert anchor_timestamps([], 1_000_000) == []


# --------------------------------------------------------------------------
# Payload shape
# --------------------------------------------------------------------------

def test_every_sample_becomes_one_timestamped_entry():
    payload = build_telemetry_payload([row(10), row(20)], ANCHOR_MS)

    assert len(payload) == 2
    assert payload[1]['ts'] == ANCHOR_MS
    assert payload[0]['ts'] == ANCHOR_MS - 10


def test_the_seven_sensor_fields_travel_under_their_csv_names():
    """Same names the historical CSV corpus uses, so a ThingsBoard chart and a
    CSV column mean the same thing."""
    values = build_telemetry_payload([row(10)], ANCHOR_MS)[0]['values']

    assert list(values) == list(TELEMETRY_FIELDS)
    assert values['x_data'] == 1
    assert values['temp'] == 7


def test_the_summary_carries_what_is_constant_across_the_burst():
    summary = build_burst_summary([row(10), row(1010)], 4100, ANCHOR_MS)

    assert summary['ts'] == ANCHOR_MS
    assert summary['values'] == {
        'battery_voltage': 4100,
        'sample_count': 2,
        'duration_ms': 1000,
    }


def test_there_is_no_summary_for_a_capture_with_no_samples():
    assert build_burst_summary([], 4100, ANCHOR_MS) is None


def test_chunking_never_emits_an_empty_block():
    assert list(chunked([1, 2, 3], 2)) == [[1, 2], [3]]
    assert list(chunked([], 2)) == []


# --------------------------------------------------------------------------
# Plausibility — what must never reach a shared history
# --------------------------------------------------------------------------

def test_a_capture_whose_timestamps_go_backwards_is_not_plausible():
    """Observed in one of four laboratory CSVs: TCP framing desynchronised and
    produced timestamps spanning 36 days. Published, it would scatter datapoints
    across weeks of ThingsBoard history, irreversibly."""
    assert is_plausible_burst([row(50), row(10)]) is False


def test_a_capture_whose_mean_interval_is_impossible_is_not_plausible():
    """The criterion is the MEAN interval per sample, not the total span: a
    legitimate burst can last minutes, and rejecting by duration would throw
    away long healthy captures at low rates."""
    too_far = MAX_MEAN_INTERVAL_MS + 1

    assert is_plausible_burst([row(0), row(too_far)]) is False


def test_a_capture_exactly_at_the_limit_is_still_published():
    """The limit is inclusive. Without this case, tightening the comparison to
    `<` would silently start discarding captures at the boundary."""
    assert is_plausible_burst([row(0), row(MAX_MEAN_INTERVAL_MS)]) is True


def test_a_long_but_evenly_spaced_capture_is_plausible():
    """20 minutes at 12.5 Hz is the shape the fleet is allowed to produce."""
    samples = [row(i * 80) for i in range(1000)]

    assert is_plausible_burst(samples) is True


def test_repeated_timestamps_are_accepted():
    """Above 1 kHz several samples land on the same millis() value."""
    assert is_plausible_burst([row(10), row(10), row(11)]) is True


def test_a_single_sample_has_no_interval_to_judge():
    assert is_plausible_burst([row(10)]) is True


def test_no_samples_is_not_a_plausible_burst():
    assert is_plausible_burst([]) is False


# --------------------------------------------------------------------------
# Publishing
# --------------------------------------------------------------------------

def test_a_burst_is_published_as_chunks_plus_one_summary():
    """Split because a broker refuses a message above its limit, and one
    capture can be 60000 samples."""
    client = FakeMqttClient()
    publisher = TelemetryPublisher(chunk_size=2, client=client)

    assert publisher.publish('10.0.0.7', [row(i * 10) for i in range(5)], 4100, ANCHOR) is None

    topics = [topic for topic, _, _ in client.published]
    assert topics == ['sif/telemetry/10.0.0.7'] * 3 + ['sif/burst/10.0.0.7']


def test_the_device_ip_is_the_device_identity_on_the_topic():
    client = FakeMqttClient()

    TelemetryPublisher(client=client).publish('10.0.0.9', [row(10)], 4100, ANCHOR)

    assert client.published[0][0] == 'sif/telemetry/10.0.0.9'


def test_an_implausible_burst_is_refused_with_a_reason_and_nothing_is_sent():
    """The local CSV keeps the raw data for diagnosis, so refusing to publish
    loses nothing — and the reason is returned rather than logged and dropped."""
    client = FakeMqttClient()

    reason = TelemetryPublisher(client=client).publish('10.0.0.7', [row(50), row(10)], 4100, ANCHOR)

    assert reason is not None
    assert 'implaus' in reason
    assert client.published == []


def test_an_empty_burst_is_refused_with_a_reason():
    client = FakeMqttClient()

    reason = TelemetryPublisher(client=client).publish('10.0.0.7', [], 4100, ANCHOR)

    # The text is asserted, not just its presence: a reason that is an empty
    # string reads as "published" to whoever records the capture.
    assert 'vazio' in reason
    assert client.published == []


def test_a_broker_that_is_merely_offline_is_not_a_refusal():
    """paho queues QoS 1 and delivers on reconnect, so MQTT_ERR_NO_CONN is not
    a lost burst. Reporting it as one would send an operator hunting for data
    that arrives a minute later."""
    client = FakeMqttClient(rc=4)   # MQTT_ERR_NO_CONN

    reason = TelemetryPublisher(client=client).publish('10.0.0.7', [row(10)], 4100, ANCHOR)

    assert reason is None


def test_a_send_that_really_failed_is_reported_as_the_reason():
    """A publish that fails must never be counted as published: the capture
    history is what tells the operator whether the data exists anywhere."""
    client = FakeMqttClient(rc=1)

    reason = TelemetryPublisher(client=client).publish('10.0.0.7', [row(10)], 4100, ANCHOR)

    assert reason is not None
    assert 'rc=1' in reason


def test_publishing_never_opens_a_socket_of_its_own_when_a_client_is_given():
    """Injection is what keeps this file runnable with no broker and no paho."""
    publisher = TelemetryPublisher(client=FakeMqttClient())

    publisher.close()   # must not try to disconnect a client it does not own
