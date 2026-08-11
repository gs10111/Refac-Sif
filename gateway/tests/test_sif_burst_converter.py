"""The gateway forwards each sample under the instant it was taken.

Acceptance criterion: a capture published to MQTT reaches ThingsBoard with one
datapoint per sample, each carrying the sensor's own timestamp. The gateway's
stock JSON converter stamps the arrival time, which would collapse a whole
capture onto a single instant — that is the reason this converter exists.

These tests exercise the pure functions and the converter's dict-shaped output,
so they run on a machine where thingsboard-gateway is not installed.
"""
import json
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

from sif_burst_converter import (          # noqa: E402
    DEVICE_TYPE, SifBurstConverter, device_name_from_topic, parse_entries,
)

TELEMETRY = [
    {'ts': 1_700_000_000_000, 'values': {'x_data': 1, 'temp': 7}},
    {'ts': 1_700_000_000_020, 'values': {'x_data': 2, 'temp': 7}},
]


def test_the_device_is_the_last_segment_of_the_topic():
    """Topics are sif/telemetry/<ip> and sif/burst/<ip>: the IP is the identity
    the fleet is known by, because it is the only key a device gives us."""
    assert device_name_from_topic('sif/telemetry/10.0.0.7') == '10.0.0.7'
    assert device_name_from_topic('sif/burst/10.0.0.7') == '10.0.0.7'


def test_a_trailing_slash_does_not_invent_an_empty_device():
    assert device_name_from_topic('sif/burst/10.0.0.7/') == '10.0.0.7'


@pytest.mark.parametrize('body', [
    json.dumps(TELEMETRY),
    json.dumps(TELEMETRY).encode('utf-8'),
    TELEMETRY,
])
def test_the_payload_is_read_whatever_shape_the_gateway_hands_over(body):
    """bytes, str or list — which one arrives depends on the gateway version and
    connector, and picking wrong would fail silently, with no telemetry."""
    assert parse_entries(body) == TELEMETRY


def test_a_single_object_is_wrapped_rather_than_iterated_as_keys():
    """The burst summary topic sends one object, not a list. Iterating it would
    yield its keys and produce nonsense."""
    summary = {'ts': 1_700_000_000_000, 'values': {'battery_voltage': 4100}}

    assert parse_entries(summary) == [summary]


def test_a_string_timestamp_becomes_an_integer():
    """ThingsBoard rejects a ts that is not a number, and JSON producers are not
    always careful about the difference."""
    entries = parse_entries([{'ts': '1700000000000', 'values': {'x_data': 1}}])

    assert entries[0]['ts'] == 1_700_000_000_000


def test_an_entry_without_a_timestamp_is_an_error_not_a_guess():
    """Guessing here means inventing when the sample was taken."""
    with pytest.raises(KeyError):
        parse_entries([{'values': {'x_data': 1}}])


def test_every_sample_keeps_its_own_timestamp_through_the_converter():
    converted = SifBurstConverter({}).convert(
        {'topic': 'sif/telemetry/10.0.0.7'}, json.dumps(TELEMETRY))

    assert converted['deviceName'] == '10.0.0.7'
    assert converted['deviceType'] == DEVICE_TYPE
    assert [entry['ts'] for entry in converted['telemetry']] == [
        1_700_000_000_000, 1_700_000_000_020]


def test_a_topic_given_as_a_plain_string_works_too():
    """Older gateway lines hand the topic itself rather than a config dict."""
    converted = SifBurstConverter({}).convert('sif/burst/10.0.0.9',
                                              json.dumps(TELEMETRY[0]))

    assert converted['deviceName'] == '10.0.0.9'


def test_a_malformed_payload_returns_nothing_instead_of_killing_the_connector():
    """The gateway keeps running for every other device; a converter that raises
    would take the subscription down with it."""
    assert SifBurstConverter({}).convert({'topic': 'sif/telemetry/10.0.0.7'},
                                         b'{not json') is None
