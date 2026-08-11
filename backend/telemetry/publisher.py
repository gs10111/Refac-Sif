"""Publishes captures to a local MQTT broker, in the shape thingsboard-gateway
forwards to ThingsBoard.

It knows nothing about sockets, CSV or ThingsBoard itself: it takes samples that
are already parsed and publishes them as JSON. The MQTT client is injectable,
which is what lets the whole module be tested without a broker — and paho is
imported only when this class is asked to build a client of its own, so a
machine that never publishes does not need the dependency installed.
"""
import json
import logging

# Indices 1..7 of every queue row. Index 0 is the timestamp; the battery is
# constant across the burst and travels in the summary instead.
#
# The names are the ones the historical CSV corpus uses (protocol/packet.py
# SAMPLE_COLUMNS), so a ThingsBoard chart and a CSV column mean the same thing.
TELEMETRY_FIELDS = (
    'x_data',
    'x_gyro',
    'y_data',
    'y_gyro',
    'z_data',
    'z_gyro',
    'temp',
)

# The sensor samples between 12.5 and 200 Hz, chosen by the server. A legitimate
# burst can last minutes — its length is the interval until the next magnet pass,
# bounded by the ring buffer and by idle_min. So the criterion is the MEAN
# interval per sample, not the total span: it catches desynchronised TCP framing
# without discarding long, healthy captures at any of the rates.
MAX_MEAN_INTERVAL_MS = 1000

# Samples per MQTT message. A broker refuses a message over its size limit, and
# one capture can be 60000 samples.
DEFAULT_CHUNK_SIZE = 100


def anchor_timestamps(raw_ts, anchor_epoch_ms):
    """Turn millis()-since-boot into absolute epoch milliseconds.

    The ESP32 restarts millis() on every deep sleep, so each capture arrives
    starting near zero. The last sample is anchored at the instant the connection
    closed and the earlier ones step back by the sensor's own deltas, which keeps
    the real spacing between samples.
    """
    if not raw_ts:
        return []
    last = raw_ts[-1]
    return [anchor_epoch_ms - (last - ts) for ts in raw_ts]


def chunked(items, size):
    """Split a list into blocks of at most `size`. No empty block is emitted."""
    for start in range(0, len(items), size):
        yield items[start:start + size]


def build_telemetry_payload(samples, anchor_epoch_ms):
    """Per-sample telemetry, in ThingsBoard's multi-timestamp form."""
    if not samples:
        return []
    absolute = anchor_timestamps([s[0] for s in samples], anchor_epoch_ms)
    return [
        {'ts': ts, 'values': dict(zip(TELEMETRY_FIELDS, sample[1:8]))}
        for ts, sample in zip(absolute, samples)
    ]


def build_burst_summary(samples, battery_mv, anchor_epoch_ms):
    """What is constant across the capture: battery, sample count, duration."""
    if not samples:
        return None
    return {
        'ts': anchor_epoch_ms,
        'values': {
            'battery_voltage': battery_mv,
            'sample_count': len(samples),
            'duration_ms': samples[-1][0] - samples[0][0],
        },
    }


def is_plausible_burst(samples):
    """Whether the samples form a capture that could physically have happened.

    TCP framing can desynchronise and produce chaotic timestamps — seen in one
    of four laboratory CSVs, with values out of order and a span of 36 days.
    Publishing that would scatter datapoints across weeks of shared history,
    irreversibly. The local CSV keeps the raw data for diagnosis, so refusing to
    publish loses nothing.

    Two criteria, both independent of how long the burst is: non-decreasing
    timestamps, and a plausible mean interval per sample. Repeated timestamps are
    accepted on purpose — above 1 kHz several samples land on the same millis().
    """
    if not samples:
        return False
    raw_ts = [s[0] for s in samples]
    if any(raw_ts[i] > raw_ts[i + 1] for i in range(len(raw_ts) - 1)):
        return False
    if len(raw_ts) == 1:
        return True
    mean_interval = (raw_ts[-1] - raw_ts[0]) / (len(raw_ts) - 1)
    return mean_interval <= MAX_MEAN_INTERVAL_MS


class TelemetryPublisher:
    """Publishes bursts to MQTT. The client is injectable; see module docstring."""

    def __init__(self, host='127.0.0.1', port=1883, topic_prefix='sif',
                 chunk_size=DEFAULT_CHUNK_SIZE, client=None):
        self._topic_prefix = topic_prefix
        self._chunk_size = chunk_size
        self._owns_client = client is None
        if not self._owns_client:
            self._client = client
            return

        # Imported here, not at module scope: the pure functions above and every
        # injected-client path stay usable on a machine without paho installed.
        import paho.mqtt.client as mqtt

        self._client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        # connect_async + loop_start does not block when the broker is down; paho
        # reconnects in the background instead of stalling the server's threads.
        self._client.connect_async(host, port)
        self._client.loop_start()

    def publish(self, device_ip, samples, battery_mv, anchor_dt):
        """Publish one burst: per-sample telemetry in blocks, then the summary.

        `anchor_dt` is the moment the connection closed — in production the naive
        local datetime the server already records for the CSV name.

        Returns None when the burst was published (or merely queued because the
        broker is offline), and the reason otherwise. The reason is returned
        rather than only logged: whoever records the capture needs to be able to
        say why nothing arrived, instead of a failure disappearing into a log.
        """
        if not samples:
            logging.warning(f'Empty burst from {device_ip}; nothing published.')
            return 'burst vazio'

        if not is_plausible_burst(samples):
            reason = (
                f'timestamps implausiveis ({len(samples)} amostras, span '
                f'{samples[-1][0] - samples[0][0]} ms)'
            )
            logging.warning(
                f'Burst from {device_ip} discarded: {reason}. '
                f'The local CSV holds the raw data for diagnosis.'
            )
            return reason

        anchor_ms = int(anchor_dt.timestamp() * 1000)
        telemetry_topic = f'{self._topic_prefix}/telemetry/{device_ip}'
        payload = build_telemetry_payload(samples, anchor_ms)

        # Keeps the first real failure but does not break the loop: the remaining
        # blocks and the summary are attempted anyway, so a broker that recovers
        # mid-burst still receives the rest.
        failure = None
        for block in chunked(payload, self._chunk_size):
            failure = failure or self._send(telemetry_topic, block)

        summary = build_burst_summary(samples, battery_mv, anchor_ms)
        failure = failure or self._send(f'{self._topic_prefix}/burst/{device_ip}', summary)

        if failure is not None:
            return failure

        logging.info(
            f'Burst from {device_ip} published: {len(samples)} samples, '
            f'battery {battery_mv} mV.'
        )
        return None

    def _send(self, topic, body):
        """Publish one serialisable body and say whether it really went out.

        MQTT_ERR_NO_CONN (4) is not a refusal: paho queues QoS 1 and delivers on
        reconnect. Any other non-zero rc is a real send failure, and its reason
        is returned so the caller can report it instead of counting it as sent.
        """
        result = self._client.publish(topic, json.dumps(body), qos=1)
        if result.rc == 0:                     # MQTT_ERR_SUCCESS
            return None
        if result.rc == 4:                     # MQTT_ERR_NO_CONN
            logging.warning(
                f'No broker connection while publishing to {topic}; '
                f'message queued (rc={result.rc}).'
            )
            return None
        reason = f'falha ao publicar em {topic} (rc={result.rc})'
        logging.warning(f'{reason}.')
        return reason

    def close(self):
        """Close the connection, if this object owns the client."""
        if self._owns_client:
            self._client.disconnect()
            self._client.loop_stop()
