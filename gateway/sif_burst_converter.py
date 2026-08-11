"""Custom uplink converter: SIF captures into ThingsBoard telemetry.

It exists to preserve the timestamp of every individual sample. The gateway's
stock JSON converter stamps the arrival time of the message, which would collapse
a whole capture — up to 60000 samples spanning minutes — onto a single instant.

Runs inside the thingsboard-gateway container, mounted at
/thingsboard_gateway/extensions/mqtt/sif_burst_converter.py. The imports below
degrade on purpose so the pure functions can be tested on a machine where the
gateway is not installed.
"""
import json

DEVICE_TYPE = 'SIF Inercial'

# The converter's return API changed across gateway lines: up to 3.5 it returns a
# dict, from 3.6 on a ConvertedData object. Detected here rather than asking the
# operator to choose at install time — the wrong choice fails silently, with no
# telemetry and no clear error.
try:
    from thingsboard_gateway.gateway.entities.converted_data import ConvertedData
    from thingsboard_gateway.gateway.entities.telemetry_entry import TelemetryEntry

    USES_CONVERTED_DATA = True
except ImportError:
    ConvertedData = None
    TelemetryEntry = None
    USES_CONVERTED_DATA = False

try:
    from thingsboard_gateway.connectors.converter import Converter
except ImportError:
    # Lets the pure functions run under pytest without the gateway installed.
    Converter = object


def device_name_from_topic(topic):
    """The device name is the last segment of the topic.

    Topics are `sif/telemetry/<ip>` and `sif/burst/<ip>`, so the last segment is
    the IP — the only identity a device hands us.
    """
    return str(topic).rstrip('/').split('/')[-1]


def parse_entries(body):
    """Normalise the payload into a list of {"ts": int, "values": dict}.

    Accepts bytes, str, dict or list: which one arrives depends on the gateway
    version and the connector. The telemetry topic sends a list of samples; the
    summary topic sends a single object, which is wrapped rather than iterated —
    iterating a dict would yield its keys.

    Raises KeyError when an entry has no `ts` or no `values`: guessing there
    means inventing when the sample was taken.
    """
    if isinstance(body, (bytes, bytearray)):
        body = body.decode('utf-8')
    if isinstance(body, str):
        body = json.loads(body)
    if isinstance(body, dict):
        body = [body]
    return [{'ts': int(entry['ts']), 'values': entry['values']} for entry in body]


class SifBurstConverter(Converter):
    """Converts SIF captures into the gateway's internal format.

    Works on both the old API (dict, gateway up to 3.5) and the new one
    (ConvertedData, 3.6+); which one is decided at import.
    """

    def __init__(self, config, logger=None):
        self.__config = config
        self._log = logger

    def convert(self, config, data):
        try:
            topic = config.get('topic') if isinstance(config, dict) else config
            device_name = device_name_from_topic(topic)
            entries = parse_entries(data)

            if USES_CONVERTED_DATA:
                converted = ConvertedData(device_name=device_name,
                                          device_type=DEVICE_TYPE)
                for entry in entries:
                    converted.add_to_telemetry(
                        TelemetryEntry(entry['values'], ts=entry['ts']))
                return converted

            return {
                'deviceName': device_name,
                'deviceType': DEVICE_TYPE,
                'attributes': [],
                'telemetry': entries,
            }
        except Exception as error:
            # Returning None drops this message. Raising would take the whole
            # subscription down, and with it every other device on the gateway.
            if self._log:
                self._log.exception(error)
            return None
