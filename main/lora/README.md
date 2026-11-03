# External LoRa extension boundary

No radio driver is initialized for D1S. No gateway client is linked in v0.1.
Keep three future adapters separate:

* Meshtastic MQTT: decode only that protocol, with explicit channel authorization.
* RNode/Reticulum: use the Linux bridge's supported interface/radio statistics.
* Raw LoRa: documented custom gateway JSON schema; only decoded packets from the
  configured frequency/modulation, never a claim to decode all RF activity.

Normalize optional values: `frequency_hz`, `bandwidth_hz`, `spreading_factor`,
`coding_rate`, `rssi_dbm`, `snr_db`, `rx_packets`, `tx_packets`, `received_at_unix`.
Missing fields remain unavailable. Bound packet/event buffers, reject duplicate
events, expire stale configuration. Build the signal timeline from genuine
gateway events, not a random waterfall. Europe 868 MHz is a gateway profile, not
a hardcoded assumption that all frequencies/modulations are lawful everywhere.
No transmit operations should be exposed by the initial adapter.
