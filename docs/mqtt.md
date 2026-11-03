# MQTT contract, schema 1

Publishing is optional. Empty MQTT URI disables the client. Works with a local
broker and needs no Home Assistant or internet access. Broker credentials are
entered on-device, stored in NVS, never hardcoded/logged. Restrict broker ACLs to
the configured device's topics; the firmware does not subscribe to remote commands.

## Topics

| Topic | QoS | Retained | Meaning |
|---|---|---|---|
| `sensecap/<device_id>/state` | 0 | no | Current climate snapshot |
| `sensecap/<device_id>/availability` | 1 | yes | `online` at connect; LWT `offline` |

Device ID accepts only ASCII letters, digits, `_`, `-` (max31 chars). Choose a
unique value per device. Publication defaults to30s. No offline state backlog;
the single-slot application queue replaces old unsent snapshots, the network
service drops disconnected publications and MQTT outbox is capped at4096 bytes.

Example only (these are **documentation examples**, not production fallback data):

```json
{
  "schema": 1,
  "uptime_s": 3600,
  "temperature": 22.8,
  "humidity": 46.2,
  "co2": 624.0,
  "voc_index": 82.0,
  "dew_point": 10.62,
  "absolute_humidity": 9.39
}
```

| Field | Unit / meaning |
|---|---|
| `schema` | integer payload version |
| `uptime_s` | monotonic device seconds, not Unix time |
| `temperature` | AHT20 ambient °C |
| `humidity` | AHT20 relative humidity % |
| `co2` | SCD41 ppm |
| `voc_index` | SGP40 Sensirion relative index, **not ppb/ppm** |
| `dew_point` | °C, Magnus water-vapour approximation |
| `absolute_humidity` | g/m3, water-vapour density |

Every measurement is numeric or **JSON null** if missing/expired. Derived values
require both source measurements fresh. At0% RH the logarithmic dew point is
undefined (`null`), absolute humidity is0. No `NaN`, fabricated zeros or substituted
SCD41 temperatures are serialized. Air quality is not a certified AQI.

Home Assistant example: `examples/home-assistant.yaml`. It uses `expire_after`
as well as broker availability, since LWT alone does not detect a stalled sensor.
The example's availability templates explicitly mark JSON-null fields unavailable.
Auto-discovery is not implemented. After changing the publish interval ensure
`expire_after` is suitably longer; units/device classes must remain correct.

## Transport security

`mqtts://` enables certificate-chain verification with the IDF CA bundle; configure
a local NTP server for a valid device clock. Private CA injection/provisioning is
a later enhancement; don't disable TLS validation. Plaintext `mqtt://` only on a
trusted/isolated LAN. The firmware never requires a public broker.

## Future telemetry/gateway inputs

Remote host telemetry and LoRa MQTT subscriptions are **not in v0.1**. Do not
publish arbitrary JSON expecting the display to ingest it. Their later contracts
must distinguish genuine source time, snapshot freshness, protocol, units and
unavailable fields. A Meshtastic packet, Reticulum interface statistic and a raw
LoRa receive event are not interchangeable payloads.
