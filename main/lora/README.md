# D1S external LoRa client

`client.cpp` connects to the separate **XIAO ESP32-S3 + Wio-SX1262** receiver over
authenticated local WebSocket. D1S has no SX1262, links no RadioLib, and
initializes no local radio.

The worker/callback own networking and a fixed assembler, 16-event telemetry
queue and single-slot radio-configuration queue. The UI owner calls `poll()` to
drain up to eight events, then renders portable `sniffer::State`. All radio
commands are receive-configuration only; no TX/replay/injection operations exist.

See [setup and verification](../../docs/lora.md),
[schema](../../docs/lora-protocol.md), and [XIAO project](../../lora-sniffer/README.md).
This is raw receiver telemetry with conservative payload classification, **not**
a Meshtastic MQTT client, RNode serial adapter or decrypting protocol gateway.
