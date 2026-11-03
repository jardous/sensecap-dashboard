# Passive LoRa extension

## Architecture and receive-only boundary

```text
XIAO ESP32-S3 + Wio-SX1262 Kit (B2B connector)
  SX1262 DIO1 interrupt → acquisition task (priority 6)
    → 32-event queue, no blocking network calls
    → network worker → four fixed send jobs → HTTP server task
    → authenticated ws://<sniffer-ip>:8080/lora
                                      ↓ local Wi-Fi
SenseCAP Indicator D1S (no SX1262)
  esp_websocket_client → 16-event queue → single LVGL/model owner
    → active profile, RSSI/SNR, observed rate, counters, activity and raw HEX
```

The radio is private to `lora-sniffer/src/radio.cpp`. There are no LoRa transmit,
replay, injection, jamming or arbitrary SPI commands in the UI or wire protocol.
An additional HAL guard aborts **before SPI** if the first opcode is SX1262
SetTx (`0x83`), SetTxContinuousWave (`0xD1`) or SetTxInfinitePreamble (`0xD2`).
RadioLib contains upstream TX implementations, but this application never calls
them; D1S does not link RadioLib at all. This is a software receive-only design,
not a physically removed transmitter or a measured RF-emission certification.
Wi-Fi necessarily transmits normal network traffic.

IRQ handling only notifies the acquisition task. That task reads the packet and
restarts reception **before** enqueueing. Full queues drop locally instead of
blocking acquisition. There is an unavoidable read/restart gap; this is not an
all-channel, all-SF monitor or an SDR.

## Board mapping

Only the **Seeed XIAO ESP32S3 & Wio-SX1262 Kit** mapping is selected. Do not reuse
it for the XIAO nRF52840, generic header-wired radios, or another XIAO variant.

| Signal | ESP32-S3 GPIO |
|---|---:|
| SCK / MISO / MOSI | 7 / 8 / 9 |
| NSS / RESET / BUSY | 41 / 42 / 40 |
| DIO1 | 39 |
| External receive enable | 38 |
| RF switch | SX1262 DIO2 automatic control |
| TCXO | SX1262 DIO3, 1.8 V |

Pin choices were checked against Seeed's one-channel-hub BSP, the Seeed module
schematic/datasheet and matching Meshtastic variant. The module permits 1.7–3.3 V
TCXO supply; 1.8 V matches Meshtastic's configuration. This is documentary
verification; subsequent XIAO boot logs also confirmed successful receive
initialization; a later matched EU868 Long Fast test received three real CRC-valid
packets. See [verification](verification.md). The native S3 HAL
uses ESP-IDF SPI/GPIO drivers, not ESP32-only register code.

References: [Seeed Kit wiki](https://wiki.seeedstudio.com/wio_sx1262_with_xiao_esp32s3_kit/),
[Seeed one-channel-hub](https://github.com/Seeed-Studio/one_channel_hub/tree/xiao-esp32s3-sx1262-bt-config),
[Meshtastic variants](https://github.com/meshtastic/firmware/tree/master/variants).

## Build and first connection

1. Build independently from the repository root:
   ```sh
   bash scripts/build.sh
   bash scripts/build-sniffer.sh
   ```
   Both helpers bootstrap exact `vendor.lock.json` sources and use the same
   digest-pinned ESP-IDF image. The XIAO project uses an 8 MB flash configuration,
   80 MHz DIO and no PSRAM dependency. Its factory application partition is
   `0x3f0000` bytes. Do not change the hardware-confirmed D1S memory/display defaults.
2. **Before flashing:** identify the XIAO by USB by-id, obtain permission and read
   a full private 8 MB backup with esptool. Keep it mode 0600 under `backups/` with
   SHA256. Confirm a suitable antenna is connected. No flash authorization for
   this new extension is implied by a previous D1S upload.
3. With native IDF 5.1.6, source `export.sh`, then explicitly run
   `idf.py -p <XIAO-USB-by-id> flash monitor` from `lora-sniffer/` **only after approval**.
   This writes bootloader, partition table and app using the generated flash
   arguments; do not flash the application BIN at address zero. For D1S follow
   [its existing backup/flash instructions](flashing.md), not this XIAO command.
   A XIAO running Arduino/TinyUSB can re-enumerate from `seeed-xiao-s3` to
   `Espressif_USB_JTAG_serial_debug_unit` when entering download mode. If Docker's
   first connection then fails with an I/O error, identify the new port/by-id and
   reopen the container with that device; its old `--device` binding may be stale.
   Do not confuse the SenseCAP's separate RP2040 `/dev/ttyACM*` with the XIAO.
4. No credentials are built in. An unprovisioned sniffer starts radio acquisition
   but **does not start Wi-Fi or an open AP**. USB provisioning is physical/local:
   ```sh
   python3 -m venv .venv-lora
   .venv-lora/bin/pip install 'pyserial>=3.5' 'websockets>=15'
   .venv-lora/bin/python scripts/provision-sniffer.py create secrets/xiao.json
   .venv-lora/bin/python scripts/provision-sniffer.py send secrets/xiao.json --port <XIAO-USB-by-id>
   ```
   `create` prompts for SSID/password and generates a random token, saving the JSON
   and adjacent `secrets/xiao.token` with mode 0600, without overwriting existing
   files. `send` intentionally writes XIAO NVS and reboots after commit. It never
   echoes serial output or secrets; close any serial monitor first. Python 3.11+
   is required for the optional observer script. Do not paste credentials into
   source, command arguments, screenshots or chat.
5. **Pair SenseCAP over USB—no token typing.** Close serial monitors and use the
   SenseCAP **CH340** port, not XIAO or RP2040:
   ```sh
   .venv-lora/bin/python scripts/provision-dashboard.py --port /dev/serial/by-id/usb-1a86_USB_Serial-if00-port0 --token-file secrets/xiao.token --host lora-sniffer.local
   ```
   The helper probes the dashboard first, sends the token privately and waits for
   an NVS-commit acknowledgement before reporting success. D1S then restarts with
   the link enabled. Only host/port/token/enabled change; Wi-Fi, host monitoring,
   radio profiles, refresh/history and reconnect preference are preserved. No
   secret is echoed. Firmware from this revision is required. Do not save UI
   settings concurrently with USB provisioning.
   Manual entry remains available under **LoRa → gear → Connection**; port
   defaults to `8080` under **Connection options**. **Save & restart** persists
   the separate `dashboard/lora_v1` blob. The general settings form remains
   separate and is available from other pages. A commit ACK is not a successful
   WebSocket/RF test.
6. Observe from a desktop without changing radio configuration:
   ```sh
   .venv-lora/bin/python scripts/lora-client.py ws://lora-sniffer.local:8080/lora --token-file secrets/xiao.token --seconds 30
   ```
   Use a manual private IP if mDNS does not work. `--raw` opts into printing full
   payload HEX; captured traffic may be private. Observe only authorized traffic.

The sniffer advertises hostname `lora-sniffer.local` and service
`_lora-sniffer._tcp` on port 8080. D1S resolves the configured hostname via lwIP
mDNS query support; it does not implement a service-browser picker. Duplicate
hostname handling is not automatic; use manual IPs for multiple sniffers.
The D1S WebSocket client reconnects after disconnects (3-second retry), while the
sniffer's Wi-Fi worker backs off to 32 seconds. Telemetry timeout shows a stale
state rather than pretending the old receiver configuration is live.

### LAN security

Use an isolated trusted Wi-Fi/LAN. `ws://` is **not encrypted**: a bearer token
authenticates the client but does not protect captured payloads or credentials
against a LAN observer/MITM. Never expose port 8080 to the internet. No cloud,
public NTP, LoRaMon, or `rns.moscow` dependency is used. NVS is also unencrypted.
Tokens require 32–95 visible ASCII characters; use the generated 64-character hex
token. Headers and payloads are never logged as credentials.

ESP-IDF 5.1 performs HTTP Upgrade **before** the application handler. Authentication
therefore happens immediately after Upgrade, before registering a telemetry
session. Bad tokens receive a fixed `Authentication failed` event and connection
closure, not necessarily HTTP 401. Two authorized sessions maximum; four fixed
send jobs. Slow clients can consume bounded send time/jobs and cause send drops,
but cannot make the acquisition task wait on sockets. This is not a hardened
multi-tenant server or denial-of-service-resistant public endpoint.

## Profiles and modes

The default is **custom 867.500 MHz / BW 125 kHz / SF8 / CR4/5 / preamble 8 /
sync 0x12 / CRC on / explicit headers**. It is not a universal mesh preset.
Reception needs matching PHY settings, including sync word and CRC/header use.
Only 863–870 MHz configurations and supported SX1262 bandwidths are accepted.
Bandwidth choices in Hz: 7810, 10420, 15630, 20830, 31250, 41670, 62500, 125000,
250000, 500000. SF5–12, CR denominators 5–8, preamble 4–65535, sync 0–255.

* **Fixed**: receive P1 continuously with brief packet read/restart gaps.
* **Scan**: rotate P1 through profile count (1–4), dwell 250–60000 ms. Every
  switch has blind periods; a short dwell can interrupt long packets. A single
  SX1262 cannot simultaneously receive different frequencies/SFs.
* **CAD**: run LoRa channel activity detection on P1. Amber events are detections,
  **not decoded packets**, bytes, authenticated peers or a broadband RSSI scanner.

### Redesigned D1S settings

Two sections replace the numeric Link/Radio/P1–P4 forms:

* **Connection**: an on/off switch, XIAO hostname/IP and masked access token.
  Port, stale timeout, refresh and history capacity are tucked under
  **Connection options**. Time fields use seconds, including fractions.
* **Receiver**: named **Fixed receive / Scan profiles / Activity only (CAD)**
  choices, a tuning-preset picker and a compact summary of the current draft.
  Scan reveals its profile count, editable profile selector and dwell time.
  Changing the selected profile preserves edits to the other profiles.
* **Edit tuning details** exposes profile name, MHz frequency (up to 1 Hz
  precision), bandwidth, SF and coding-rate dropdowns, hex sync word, preamble and
  a CRC switch. There are no numeric mode/boolean codes or decimal sync-word
  conversions. Existing custom values remain intact; choosing a preset explicitly
  replaces only the currently edited profile.

**Apply radio** queues a live receive configuration for the currently connected
XIAO, without saving it. It is disabled when offline or when the address/token
draft differs from the saved connection. A queued message is not a hardware
ACK—check the LoRa page's active profile and radio-ready state.
**Save & restart** persists the D1S settings and restarts only after commit;
it does not imply a live radio apply. Turn on **Restore radio on reconnect** in
Receiver if the saved radio setup should be sent after restart/reconnection.
Close/Cancel discards the draft. Advanced controls scroll; action buttons and
validation messages stay visible. Background navigation remains blocked.

The redesign has firmware-build and 6/6 normal plus 6/6 sanitizer desktop
verification, including preset/unit mappings, multi-profile drafts, invalid input,
masked token, save/cancel and offline apply. **It has now been flashed and
boot-checked on D1S**; physical interaction confirmation is still pending.
Sniffer radio settings themselves are RAM-only and return to defaults at boot.
Last valid authorized client command wins; do not configure from competing clients.

For comparison, documented **Meshtastic EU868 LongFast** uses **869.525 MHz,
BW250 kHz, SF11, CR4/5, sync0x2B, preamble16**, CRC enabled. Enter these explicitly
if that is the genuine transmitter profile; custom channels/presets can differ.
For a default EU868 Long Fast node, use **LoRa → gear → Receiver → Tuning preset
→ Meshtastic EU868 Long Fast**, with **Fixed receive**, then **Apply radio**.
To retain this choice through XIAO/D1S restarts, enable **Restore radio on
reconnect** and use **Save & restart**. This does not require re-entering the
paired token. A live apply alone is RAM-only. No channel encryption key is needed
to receive packet bytes; encrypted message text is not decoded by this dashboard.
MeshCore and RNode installations likewise need their actual configured PHY—no
universal preset is inferred from their protocol name or a frequency.

## UI semantics and classification

* RSSI/SNR are the **last successfully received packet's** values, not live noise
  floor. Mini charts show recent packet samples, not evenly timed RF measurements.
* `RECEIVED` shows the sniffer's since-boot RX counter; tap it for all counters.
  CRC failures are not successful packets and their payload is never displayed.
* Rate is this D1S's observed successful packet events per second over the **last
  60 seconds**, including empty time. Server counter and local history can differ.
* 60s/5m/15m bars use D1S receive times; cyan=packets, amber=CAD detections.
  Each has 60 bins. The oldest history is aged out, not replayed after reconnect.
* Recent list capacity is 1–32; tap a row for sniffer-uptime timestamp, profile,
  length, CRC, RSSI/SNR, classification/confidence and complete uppercase HEX.
  No UTC or decrypted content is claimed. History is RAM-only; boot-ID changes
  reset the packet ring/activity/deduplication state.
* Queue/send/UI drops are known local pipeline losses. They are **not a count
  of all missed over-air packets**. No-client broadcasts do not count as drops;
  acquisition events discarded during Wi-Fi loss do. Local counters reset on D1S
  reboot; sniffer counters on XIAO reboot.

Portable `PacketClassifier` plugins inspect payload structure. All results are
**Unknown or Probable** (`?` in list labels); **Confirmed is never asserted**.
Frequency alone is not evidence. No keys, decryption or signature validation:

* Meshtastic: documented 16-byte header plausibility + matching sync0x2B.
* Reticulum/RNode: plausible unframed H1 announce with public-key/hash/signature
  layout and sync0x12. Generic/encrypted DATA, IFAC, and RNode fragmentation are
  not decoded/reassembled. This is not an RNode serial/KISS adapter or a peer list.
* MeshCore: structural version-0 ADVERT candidate including optional transport
  codes and encoded path hash widths. Signature remains unverified.
* Multiple matching classifiers produce **Unknown**, not guessed precedence.

References: [Meshtastic radio settings](https://meshtastic.org/docs/overview/radio-settings/),
[radio header](https://meshtastic.org/docs/overview/mesh-algo/),
[MeshCore packet format](https://github.com/meshcore-dev/MeshCore/blob/main/docs/packet_format.md),
[payloads](https://github.com/meshcore-dev/MeshCore/blob/main/docs/payloads.md),
[Reticulum 0.9.1 Packet.py](https://github.com/markqvist/Reticulum/blob/0.9.1/RNS/Packet.py).
False positives are possible even at Probable confidence.

## USB versus Wi-Fi

The XIAO **does have native USB serial** (ESP32-S3 USB Serial/JTAG). Current
firmware uses it for local credential provisioning, boot output and a read-only
connection diagnostic console. Run `.venv-lora/bin/python scripts/sniffer-serial.py
watch` from the repository root for live Wi-Fi state/RSSI/IP/retry/disconnect
details without Wi-Fi or a token; see [USB commands](../lora-sniffer/README.md#usb-connection-diagnostics).
Structured packet telemetry and receive-configuration commands currently use the
authenticated Wi-Fi/WebSocket API; there is no general USB JSON packet/control
adapter yet (USB status snapshots do include radio readiness and counters). That
is a firmware boundary, not a hardware limitation, and the
radio can acquire packets while Wi-Fi is disabled. Direct USB use by D1S would
also require a supported USB-host path/client, not simply connecting two device
ports. No USB data-transport option is presented as implemented in the settings.

Separately, **SenseCAP's CH340 UART0** now accepts narrowly scoped local USB link
setup through `scripts/provision-dashboard.py`. RX is buffered/bounded, tokens
are never echoed and a single asynchronous save owner commits only `lora_v1`.
Successful commit triggers restart; failure leaves the running settings active.
The handshake avoids accidentally sending a token to other firmware, but is not
cryptographic device attestation. Trust the physical USB device/host. This setup
channel is not packet telemetry, a general remote shell, or a way to control the
SX1262 directly. RP2040 acquisition remains on UART2.

## Verification boundary (2026-11-03)

**Done:** both ESP-IDF builds; 7/7 desktop suites; 7/7 ASan/UBSan suites (LVGL
process-lifetime leak checking excluded); shared-LVGL simulated/unavailable UI;
wire round trips, bounds/duplicate/NUL/depth rejection, fragments/control frames,
bearer comparison, classifiers/ambiguity policy, history/aging/deduplication and
legacy settings-layout tests; private provisioning-file/tool tests; deterministic
mutation/random-input tests.
Those desktop suites are distinct from the live checks recorded below.

**D1S upload:** user-authorized flash completed with all three image hashes
verified, after a new private 8 MB backup (`sensecap-before-lora-20261103-151227.bin`).
A 45-second boot capture confirmed PSRAM at 120 MHz, XIP, memory test OK, and
real UART temperature/RH/CO2 samples. No panic/reset loop was observed. VOC was
not yet observed during this short capture. Saved Wi-Fi was used, but association
attempts initially failed; the subsequent host-editor/modal corrective upload
booted and successfully acquired a local Wi-Fi/DHCP address.
Physical display/touch acceptance of this new build still needs user confirmation.

**XIAO upload:** antenna attachment and flashing were explicitly approved. The
original 8 MB image was backed up privately as
`backups/xiao-before-sniffer-20261103-152306.bin` with SHA256. Bootloader, partition
table and 852288-byte application writes all passed hash verification. Initial
USB reset/log capture left the device in ROM download mode; after the user
reconnected it, a 20-second capture showed `lora_sniffer` v0.2.0 and successful
SX1262 RX initialization: 867500000 Hz, SF8, BW125000, CR4/5, sync0x12, fixed mode.
No panic or radio-configuration error appeared. SenseCAP is now disconnected;
it was not touched during the subsequent XIAO checks.

**Provisioning/live link:** with explicit user approval, Wi-Fi credentials were
reused locally from the private SenseCAP NVS snapshot after validating page,
entry and blob CRCs. A new random token was saved in ignored mode-0600
`secrets/xiao.json` and `secrets/xiao.token`; neither password nor token was
printed. USB provisioning acknowledged NVS commit/reboot; later boots loaded it.
The XIAO joined Wi-Fi, acquired `192.168.0.57`, and `.local` name resolution was
observed. A desktop client received advancing authenticated status messages with
`radio_ready:true`, completed ping/pong, and verified that a wrong token receives
the fixed authentication error and closure without telemetry. RX counter was 0.

Live testing also exposed two corrections: Wi-Fi now advertises PMF capability
like the working D1S and avoids overlapping connect attempts. It scans all
matching APs, permits three retries per AP, and bounds each connection attempt
to 30 seconds before backoff. The HTTPD wrapper
does not call `httpd_ws_recv_frame(...,0)` a second time for an empty payload.
That API means “read a header”, so empty final continuation/ping/close frames
previously waited for another frame header. Both corrections were built/flashed
with all image hashes verified; desktop/sanitizer suites remain 6/6 passing.
Serial-open/reset operations repeatedly restart this native-USB XIAO, and the AP
sometimes refuses reassociation/authentication temporarily; long-run Wi-Fi
reliability is not qualified. Network-only checks avoid those extra USB resets.

**Earlier retest outcome (before USB diagnostics/profile matching):** that corrective image booted, initialized fixed
RX and obtained DHCP, but the desktop could not reliably reach its TCP endpoint
(`192.168.0.57`, neighbour/ARP resolution failed). The comprehensive latest-image
retest therefore did **not** complete. The authenticated-stream/ping/wrong-token
passes above were observed before the final empty-frame/AP-retry correction;
do not treat them as full qualification of the final image. No scan/CAD command
was confirmed applied in the final retest; startup remained the default fixed
profile. Check AP/mesh/client-to-LAN reachability on a known-good local network
before retrying, without weakening security or exposing the service publicly.

**Later follow-up:** the Linux diagnostic helper's port-open reset was corrected;
desktop authenticated status and empty ping/pong passed after reconnection, and
the user confirmed XIAO was visible on SenseCAP. Matching the user's confirmed
EU868 Long Fast source then produced three real CRC-valid packet events over the
authenticated WebSocket. No message decryption is claimed. See
[current verification](verification.md) for measurements and remaining boundaries.

**Not done:** CAD against a real source, scanning reception,
D1S packet-row/sequence comparison, reconnect under
AP loss, slow-client stress, full-task memory/stack
qualification or RF/24h soak. The existing D1S display/touch was hardware-confirmed
**before** this extension, not yet re-qualified with it.

**Automatic SenseCAP pairing:** the USB-setup build was flashed with all three
image hashes verified, after a private current-NVS backup. USB handshake and
NVS-commit ACK passed. Flash readback CRCs passed; the legacy settings blob was
byte-for-byte unchanged, the stored token matched `secrets/xiao.token`, and
radio/history/refresh/timeout/reconnect preferences were unchanged. Post-restart
Wi-Fi/DHCP and real UART sensors worked. WebSocket attempts still timed out
before authentication; endpoint reachability is not claimed fixed by pairing.

Before acceptance, back up/flash approved targets, provision privately, confirm
`radio_ready`, observe genuine matching transmissions, compare desktop/D1S
sequence and counters, disconnect/reconnect AP, test wrong token and oversize/
fragmented commands, test scan/CAD separately, then soak with Climate/Network
and Wi-Fi/MQTT active. Absence of packets alone does not prove a receiver fault.
