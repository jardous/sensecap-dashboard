# LoRa WebSocket protocol v1

Endpoint: `ws://<sniffer-ip>:8080/lora`, text messages, max **2048 bytes**, decoded
JSON depth <=8, <=192 nodes, unique keys, no embedded/escaped NUL or trailing
non-whitespace. Authorization header: `Authorization: Bearer <token>`.
See [security limitations and setup](lora.md). No unauthenticated telemetry.

## Receiver → client

Every event has `schema:1`, `type`, and 16-hex-character `boot_id`. Except `error`,
all include `timestamp_ms` (sniffer uptime, not UTC), `mode` (`fixed`, `scan`, `cad`),
`profile_index` (0–3), and the **flat profile fields** below. Each packet carries
its own profile; later scan changes cannot relabel old packets.

```json
{"schema":1,"type":"packet","boot_id":"0123456789abcdef","timestamp_ms":123456,"mode":"fixed","profile_index":0,"receiver_profile":"Custom 867.5","frequency_hz":867500000,"bw_hz":125000,"sf":8,"cr":"4/5","sync_word":18,"preamble":8,"crc_enabled":true,"sequence":1,"rssi_dbm":-99.5,"snr_db":4.25,"length":3,"crc":"ok","payload_hex":"01A0FF"}
```

Packet `sequence` increases across successful receives since this boot. It is
assigned before enqueueing, so sequence gaps can reflect local losses. `length`
is 0–255; HEX must have exactly twice that many valid hex digits. RSSI -200..20
dBm, SNR -40..30 dB, finite numbers only. `crc` is `ok` with CRC enabled or
`disabled` with CRC disabled. Failed-CRC bytes are not packet events. Integer
timestamps/sequences/counters are bounded to 2^53-1 for exact JSON numbers.

`status` is published roughly every second, even without packets. `radio_config`
reports a reconfiguration/scan switch (including failure via `radio_ready:false`).
`statistics` and `scan_status` are accepted schema-compatible types; the current
server uses `status` and `radio_config` for these updates. These four types carry:

```json
{"radio_ready":true,"rx_packets":0,"crc_errors":0,"queue_drops":0,"send_drops":0,"cad_checks":0,"cad_detections":0}
```

Add those fields to the common flat envelope/profile above, replacing packet-only
fields. Counters are cumulative for the current XIAO boot. `queue_drops` counts
full acquisition queue events; `send_drops` counts exhausted jobs, queue-work or
socket send failures, and discarded events while Wi-Fi is down. A send job can
fail for a client independently; neither counter equals all over-air loss.

`cad` carries common envelope/profile plus `detected:true`. Only detections are
streamed, not every clear CAD check; cumulative checks/detections are in status.
CAD has no payload, RSSI/SNR or packet sequence.

`error` has only schema/type/boot ID and fixed printable `message`, e.g.:
```json
{"schema":1,"type":"error","boot_id":"0123456789abcdef","message":"Authentication failed"}
```

The server does not send classification guesses; D1S classifies raw payloads
locally. No protocol/decryption/authenticated identity field is fabricated.

## Client → receiver

Only `set_config` is accepted, after authentication and at most one valid command
per second per session. The command queue has one slot. Unknown top-level or
profile keys, duplicate keys, bad types or invalid ranges are rejected.

```json
{"schema":1,"type":"set_config","mode":"fixed","dwell_ms":3000,"profiles":[{"receiver_profile":"Custom 867.5","frequency_hz":867500000,"bw_hz":125000,"sf":8,"cr":"4/5","sync_word":18,"preamble":8,"crc_enabled":true}]}
```

`profiles` contains 1–4 objects. Fixed/CAD use index0; scan rotates through the
array. Profile/range restrictions are documented in [setup](lora.md).
`cr` encodes as `"4/5"`–`"4/8"`; schema1 also accepts numeric denominators 5–8.
Explicit-header mode is fixed; implicit header settings are not exposed.

Accepted commands are asynchronous: the next `radio_config`/`status` shows active
profile and `radio_ready`; there is no request-ID ACK transaction or durable
sniffer radio-config store. No arbitrary operations or RF transmit verbs exist.

## Framing and client state

RFC6455 text/continuation frames and transport-buffer chunks are reassembled into
one fixed buffer. Control ping/pong can interleave without corrupting it. Binary,
oversized, overlapping, missing-offset or mismatched chunks are rejected. HTTPD
consumes complete frames; D1S additionally handles 512-byte transport chunks.
All server WebSocket sends execute on the HTTP server task, including broadcasts.

On D1S, schema errors increment `malformed`, full telemetry queue increments
`local_drops`. Deduplication is `(boot_id, sequence)`; a boot change resets remote
history/counters but not D1S transport-loss diagnostics. Recent packet storage is
32 maximum; 900 tagged one-second bins support 15-minute activity. Reconnect does
not request/replay old events. Packet/CAD activity timestamps use D1S receipt
time, while packet details show the original sniffer uptime.

## SenseCAP local USB pairing (not a WebSocket command)

The D1S CH340 UART0 console accepts newline-terminated JSON, at most511 bytes
before newline; overflow/NUL-containing lines are discarded and resynchronize at
the next newline. A fixed read-only probe precedes secret transfer:

```json
{"schema":1,"type":"sniffer_setup_probe"}
```

It responds with `DASH_USB_LINK_READY`. The local provisioning request is:

```json
{"schema":1,"type":"provision_link","host":"lora-sniffer.local","port":8080,"token":"<32-95 visible ASCII characters>","enabled":true}
```

All six fields are mandatory; duplicate/unknown keys, trailing data, escaped NUL,
bad schema/types/ranges and radio-operation fields are rejected. Only the four
link properties change. The existing `ClientSettings` radio configuration,
history/refresh/timeout and reconnect policy are retained. The generic settings
blob is never rewritten by this USB operation.

Fixed markers are `DASH_USB_LINK_QUEUED`, then `DASH_USB_LINK_SAVED` only after
successful NVS commit; the device restarts afterward. Invalid/busy/save failure
uses `DASH_USB_LINK_INVALID`, `DASH_USB_LINK_BUSY`, or `DASH_USB_LINK_FAILED`.
No request or secret is echoed. Writes are rate-limited; only one save can be
pending/in-flight across UI and USB. Probe is not an authentication/attestation
scheme; USB is a physical trusted provisioning boundary. WebSocket's `set_config`
parser never accepts these USB commands. Use the helper and a mode0600 token file,
not a pasted secret in terminal arguments or chat.

## XIAO local USB diagnostics (not WebSocket commands)

Native ESP32-S3 USB Serial/JTAG,115200, LF/CRLF, local echo off. Commands are
`help`/`?`, `wifi`, `status`, `watch on`, `watch off`. The command buffer accepts
at most511 bytes; overflow/NUL lines are discarded until newline, without echo.
Responses are complete JSON lines mixed with ordinary ESP-IDF boot/event logs.
All diagnostic responses have `schema:1`; `usb_help` lists commands, `usb_error`
has a fixed message, and `usb_watch` acknowledges watch-off. `wifi` emits
`type:usb_wifi`; `status` adds the existing schema1 radio `status` event. Neither
emits packet payloads. The Wi-Fi object is a separate diagnostic schema, not a
valid D1S WebSocket event. Its current fields include:

* `state`: unprovisioned/starting/connecting/waiting_for_ip/connected/disconnected.
* `configured`, `associated`, `ip_ready`, `ssid`, `mac`, `bssid`, live `rssi_dbm`,
  `channel`, numeric `auth_mode`/`power_save` from IDF. No AP measurement yields
  null RSSI/channel/auth mode and an empty BSSID; disconnected IP fields are empty.
* `ip`, `gateway`, `netmask`, `dns`; `hostname`, `port`, `path`, `server_ready`.
  Server-ready means listener initialization succeeded, not endpoint reachability.
* `attempts` (explicit connect calls), `associations`, `ip_acquisitions`,
  `disconnects`, `attempt_timeouts` (local30s watchdog), `last_connect_error`
  (connect API result), `attempt_age_ms`, `connected_ms`, `retry_in_ms`.
* `ws_authenticated`/`ws_auth_rejected` cumulative session counts and `ws_clients`
  currently open authenticated sockets, which can lag a link failure until close.
* `recent_disconnects`: maximum8 newest-first records with XIAO uptime `at_ms`,
  numeric `reason`, symbolic `name` (UNKNOWN preserves the number), and driver's
  disconnect-event `rssi_dbm` (0 can mean unavailable).

Counters/history are volatile per boot. Snapshots are observational, not an atomic
transaction across the Wi-Fi driver and event task. No credentials are included,
but SSID/MAC/local addresses are local information. Diagnostics require trusted
physical USB access, not a Wi-Fi token. `watch on` emits at1Hz with a60s lease;
repeat to renew. Replies are rate-bounded and USB RX/TX buffered. No acquisition
queue consumer, radio command, scan, reconnect or reset operation is added by
these read-only verbs. Existing strict `provision` JSON is retained separately.

`boot_id`, `uptime_ms`, numeric ESP-IDF `reset_reason`, `free_heap` (bytes) and
`console_stack_min_free` (minimum remaining console-task stack bytes since boot)
help distinguish a connection failure from a reboot/resource problem.
`event_stack_min_free` records the Wi-Fi/IP event task's minimum remaining stack
at its most recent callback. Serial-port open/close may itself reset this board
on some hosts; counters must be compared only within the same boot ID.
