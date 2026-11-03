# Verification and release status

Development snapshot: **2026-11-03**, including both flashed LoRa-extension devices;
XIAO RX initialization and three real CRC-valid packets confirmed after matching
Meshtastic EU868 Long Fast. No production qualification claimed. Earlier D1S
display/touch confirmations refer to the original stable firmware, not the new build.

## Commands executed successfully

* Inspected/cloned both official Seeed repositories at `vendor.lock.json` revisions.
* Firmware build in `espressif/idf:v5.1.6`, GCC12.2, bundled LVGL8.3.1:
  `idf.py build`; bootloader, partition table, ELF and application BIN produced.
  Latest recorded D1S app size **0x19e5f0 bytes**, fits 0x3f0000 partition
  with **59% free**. Separate XIAO USB-diagnostics app **0xd9ff0 bytes**, **78% free**.
  Final generated artifacts are authoritative if rebuilt.
* Initial `idf.py size` measurement: static D/IRAM about188KiB, about158KiB left
  before runtime heap allocations (not a measured runtime free-heap guarantee).
  This predates the XIP corrective build; do not use it as the final runtime
  internal-heap minimum. RGB buffers are in PSRAM.
* Compiler command checked: final option `-std=gnu++17`, overriding IDF's default.
* Desktop Debug CMake build with real bundled LVGL and SDL2.
* `ctest --test-dir build-desktop --output-on-failure`: **7/7 suites passed**.
* AddressSanitizer + UndefinedBehaviorSanitizer Debug build:
  `ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-sanitize --output-on-failure`:
  **7/7 suites passed**. Process-lifetime LVGL allocation leaks were not assessed.
* `python3 -m unittest discover -s bridge/tests -v`: **6 tests passed**.
* Headless shared-LVGL previews generated in `docs/previews/`, including real
  unavailable states. Climate/Network previews use clearly labeled synthetic
  desktop fixtures; LoRa has both unavailable and explicitly simulated previews.

## Test coverage

1. Dew point/absolute humidity reference values, invalid inputs, RH0/100%, NaN.
2. Little-endian float/COBS packets, all four real sensor types, unknown type,
   truncation, overflow, non-finite/out-of-range values, VOC warm-up rejection,
   fragmented stream re-synchronization and100000 random corrupt bytes.
3. Per-field freshness/future timestamps, missing-air-quality distinction,
   threshold boundaries, independent per-metric history, bucket averages,
   1h/6h/24h projection, startup gaps and24-hour rollover.
4. Settings defaults, bounds, termination, NaN thresholds and topic-injection ID.
5. Bridge projection/allowlist, secret suppression, no invented peers, nulls,
   token file permissions, command-failure handling and actual HTTP tests for
   authentication, allowed peers,404,405 and no-store headers.
6.40 repeated UI cycles through all tabs, climate detail modal, settings form,
    keyboard focus/close and missing/stale paths, both simulated and unavailable;
    LoRa ranges, counters, maximum-length raw packet details and the redesigned
    Connection/Receiver settings sections.
7. LoRa event/config round trips, wire bounds, malformed/duplicate/deep/NUL JSON,
   continuation/chunk/control framing, bearer comparison, classifiers, bounded
   history/deduplication/aging, legacy NVS layout and deterministic fuzz inputs.
8. Offline provisioning byte/type/permission/symlink/overwrite checks and observer
   summaries without raw payload, binary/schema/auth-error rejection. No serial or
   WebSocket connections are opened by the tool tests.
9. Real desktop pointer sequences for all four host long-press editors,
   short-click/Edit/empty-slot paths, selected-slot save and cancel isolation;
   modal background hit testing with no tab pressed state/navigation or changed
   navigation framebuffer pixels. Physical UI acceptance is separate.
10. Friendly LoRa setup: unit parsing/1Hz precision, hex sync, known PHY preset
    values, named mode/SF/BW/CR choices, scan-profile draft retention, invalid-input
    rejection, masked tokens, offline/changed-target apply disabling and isolated
    save/cancel. Previews use the actual LVGL renderer, not live device credentials.
11. USB link setup: scoped/atomic parser, no radio/preferences mutation, invalid
    schema/types/unknown/duplicate/NUL fields, bounded line overflow/resync, probe
    and private-file host helper. Wrong-device mock proves no token is sent before
    the dashboard handshake; save failure never becomes a success ACK.
12. XIAO USB diagnostics: allowlisted command parsing, reason names/numeric
    fallback, escaped SSIDs, no credential fields, null disconnected RSSI/empty
    disconnected addresses, bounded8-event history, too-small JSON output,
    line overflow/resync; host-tool fragmented/noisy/oversize input, terminal-safe
    summaries, status-only polling and old-firmware timeout.

### XIAO USB diagnostics upload

User approved XIAO-only installation. Current NVS was privately snapshotted as
`xiao-nvs-before-serial-diagnostics-20261103-173145.bin` with SHA256 after confirming
MAC10:b4:1d:e9:14:5c. All three image hashes verified; flash ranges excluded NVS.
First live USB request returned structured Wi-Fi state and radio-ready status
without network access or a token. SenseCAP/RP2040 firmware was not changed.

The first hardware soak exposed a new console **main-task stack overflow**;
desktop tests did not catch this embedded stack limit. That image is not accepted.
The4096-byte console-only formatting scratch buffer was moved to BSS rather than
increasing task priority or borrowing the acquisition stack. The corrected image
was reflashed with all three hashes verified
(`xiao-serial-stack-fix-flash-20261103-173516.log`). USB Wi-Fi snapshots now include
boot ID, reset reason, free heap and minimum console-stack headroom, so restarts
and memory pressure are visible instead of silently looking like reconnects.

A subsequent test reached AP association/DHCP and exposed insufficient stack in
the default2304-byte `sys_evt` task. XIAO-only event stack is now6144 bytes, in both
defaults and the generated build config; this does not change D1S or acquisition
priorities. Final upload (`xiao-serial-final-flash-20261103-173842.log`) passed all
three image hashes. NVS readback CRCs and the exact `sniffer/provision` blob match
the before-image, proving SSID/password/token preservation. Raw NVS changed
outside that blob; whole-partition identity is **not** claimed.

The final live test passed help/status/wifi, watch on/off, malformed command,
overflow/NUL rejection and recovery. It collected93 Wi-Fi and92 radio snapshots
over approximately98 seconds, with one boot, monotonic uptime, no panic/brownout,
and radio-ready throughout. Minimum remaining stack: console3120 bytes,
event3740 bytes. Wi-Fi advanced through connecting/waiting-for-IP/connected and
reported192.168.0.57, channel11, about−52dBm, one connection attempt and no
disconnect/attempt timeout during the capture. Two authenticated WebSocket
sessions were counted, but no claim of sustained D1S telemetry or RF reception
is made. The private accepted raw capture is
`/tmp/opencode/xiao-serial-accepted-live.log`. No active serial monitors remain.

Follow-up: the user saw `connecting | signal unavailable | channel None` even
about1.5m from the router. A65s capture reproduced Wi-Fi authentication retries,
including reason15 (`FOUR_WAY_HANDSHAKE_TIMEOUT`) at−55dBm, before eventually
reaching association. This does not establish the exact AP/firmware cause and
is unrelated to the application token. `None` now displays as `unavailable`.

Also confirmed the old host opener itself caused `USB_UART_CHIP_RESET`, despite
requesting both modem lines false. The Linux helper now changes DTR/RTS in one
combined ioctl (rejecting asymmetric reset/BOOT states), and clears HUPCL rather
than dropping lines on close. Four live open/close cycles kept one boot ID and
monotonic148127–148738ms uptime; Wi-Fi remained connected at−50 to−52dBm and
192.168.0.57. Unit coverage checks both pyserial hooks, combined ioctls and hangup
flags. All7 normal and7 sanitizer suites pass. No firmware/settings changed.

Desktop TCP8080 still timed out with a failed ARP entry; strong signal and an
assigned IP therefore do not establish LAN reachability. XIAO subsequently
disappeared from USB enumeration during this investigation; no reason for that
USB disappearance was established. Do not claim a complete Wi-Fi/link fix.

The user then confirmed the antenna looked connected and reconnected the XIAO.
The corrected no-reset helper reported a powered-on boot at41754ms, connected
at−52dBm/channel11,192.168.0.57, one attempt/no disconnects and one authenticated
socket open. A subsequent desktop test **passed TCP8080, authenticated WebSocket
radio-ready status and empty ping/pong**. These are current positive observations,
not a root-cause diagnosis or long-run reliability result. An attempted extra
25s capture stopped at the serial-exclusive lock because another monitor owned
the port; it did not run and no stability result is claimed. Existing monitor
was left untouched. No further firmware/settings writes were made.

The user subsequently confirmed **“now I can see it in the sensecap as well”**.
This confirms the dashboard can now see the XIAO after reconnection, alongside
the desktop authenticated-status/ping checks. It is not confirmation of actual
LoRa packet reception, sustained Wi-Fi reliability, or a fix for the separately
reported intermittent screen/input behavior.

### Matching the user's Meshtastic transmitter

After the dashboard connection worked, the user sent Meshtastic messages but saw
no packets. A live authenticated XIAO status confirmed it was still on the
original custom867.5MHz/SF8/BW125/sync0x12 profile, radio-ready with zero RX/CRC
errors/drops. The user confirmed **EU868 / Long Fast**. One authenticated
receive-only `set_config` applied fixed869.525MHz/BW250/SF11/CR4/5/sync0x2B,
preamble16/CRC on; the resulting receiver event confirmed those exact settings
and radio-ready. No firmware, NVS, token or transmit operation was involved.
This live profile remains RAM-only unless saved/restored through D1S settings.
Packet reception is not inferred from the configuration ACK; a bounded passive
observation is separate from this profile-match check.

The subsequent bounded authenticated observation received **three genuine packet
events**, sequences1–3, lengths58/39/39 bytes at869.525MHz/SF11/BW250, all CRC OK.
RSSI−68/−17/−18dBm, SNR5.75/6.5/6.5dB. It also received58 status events; the most
recent status before packet3 reported RX2, zero CRC errors and zero queue/send
drops. The status counter naturally lagged the third packet event. The observer
stopped after packet3 and did not log payload contents or send RF traffic.
This confirms real reception on the matching PHY and authenticated desktop packet
telemetry. It does not authenticate the encrypted packet's sender/protocol,
decrypt message text, or independently confirm D1S displayed these packet rows.

The user then confirmed **“yeah - now it shows something”** on SenseCAP, providing
physical acceptance of basic packet display. Exact packet-row/sequence comparison
and sustained reception remain separate checks.

### Button-only screen blanking

User requested no automatic screen blanking: only the physical button should
blank/wake. The D1S GPIO38 user button now queues a debounced press to the LVGL
owner, which toggles only backlight/input enable. Idle time and dark-screen touch
cannot blank/wake it. Sensor/network/LoRa tasks are not powered down. The legacy
`screen_timeout_s` member is retained in its exact NVS layout but ignored; new
defaults use0 and the obsolete timeout editor is replaced by an explanation.

All7 normal and7 ASan/UBSan suites passed, including nonzero legacy timeout,
manual blank/brightness restore, ignored dark-screen taps and a finger held over
navigation across wake, followed by normal fresh-touch navigation. The physical
button still requires user acceptance; it cannot be pressed by these tests.

User explicitly approved and then requested the upload. Current NVS was privately
backed up as `sensecap-nvs-before-button-screen-20261103-181136.bin` with SHA256;
MAC74:4d:bd:81:a4:6c and NVS-excluding flash ranges were checked. App1697264 bytes
(`0x19e5f0`), all three image hashes verified.25s passive post-flash capture showed
the UI timing task running at20–30s uptime, about50.7–52.1ms handler maxima and no
panic/stack-overflow/brownout. Startup/sensor-first logs preceded this capture;
they are not claimed re-observed. Panel/memory/input timings remain unchanged.
XIAO/RP2040 firmware and NVS were untouched. No extra page-navigation tracing was
included, in keeping with the user's request to flash button-only blanking.

### Automatic USB pairing upload/readback

User approved automatic pairing rather than manually entering the token. The
USB-setup firmware was flashed with all three image hashes verified after a
fresh private NVS snapshot (`sensecap-nvs-before-usb-link-20261103-171047.bin`).
The helper completed a real UART0 probe, sent the local token without displaying
it and received `DASH_USB_LINK_SAVED` after commit/restart. A separate flash readback
(`sensecap-nvs-after-usb-link-20261103-171442.bin`) passed NVS CRC checks and proved:
the legacy settings blob was byte-for-byte unchanged; host/port/enabled/token
matched the intended private request; radio profiles, history, refresh, timeout
and reconnect preference were preserved. Both snapshots remain private/ignored.
Post-restart real UART sensors and saved Wi-Fi/DHCP worked. WebSocket attempts
still timed out before authentication, so pairing is verified but the previous
endpoint reachability problem is not claimed resolved. XIAO/RP2040 unchanged.

### LoRa settings redesign upload

Two Connection/Receiver sections replace the old six numeric-coded forms.
Advanced options are collapsible; visible values use MHz, seconds, hex sync and
named dropdowns/switches. Separate Apply radio and Save & restart actions explain
live versus persisted effects. Original NVS layouts, display timing and hardware
boundaries are unchanged. The new UI passed firmware build and all6 normal plus
all6 ASan/UBSan suites; shared-LVGL previews are under
`docs/previews/lora-settings-{connection,receiver}.png`.

User-authorized SenseCAP upload completed with all three written-image hashes
verified (1693152-byte app). Current NVS was first backed up privately as
`backups/sensecap-nvs-before-lora-redesign-20261103-165424.bin`, with SHA256.
No NVS erase was performed. A 30s boot capture confirmed 120MHz PSRAM, memory
test OK, saved Wi-Fi/DHCP and real temperature20.65°C, RH52.64%, CO21591ppm;
no panic/reset loop observed. XIAO/RP2040 firmware remained untouched. Physical
interaction acceptance of the redesigned settings remains pending.

No automated physical touch, RF, actual MQTT broker or actual RNS service test
is claimed by these suites.

## Physical device observations

* USB eventually enumerated after cable/connection correction: internal hub,
  CH340 `/dev/ttyUSB0` and Seeed Indicator RP2040 `/dev/ttyACM0`.
* `esptool.py --chip esp32s3 --port /dev/ttyUSB0 flash_id` succeeded: ESP32-S3
  QFN56 revision0.2, embedded8MiB PSRAM, external8MiB flash,40MHz crystal.
* The read-only chip probe uploads a volatile stub and resets ESP32. RP2040 USB
  disconnected during a simultaneous passive log capture; this is **not** a
  confirmed sensor failure. Repeat log capture after all probes complete.
* Full ESP32 flash read succeeded:8388608 bytes, local private backup at
  `backups/esp32-before-dashboard-20261103.bin`, SHA256 recorded in its adjacent
  `.sha256` file; both mode0600 and ignored by Git. A second independent read
  comparison has not been performed.
* Passive RP2040 USB capture **with DTR enabled at115200** confirmed real AHT20
  temperature23.34°C/RH46.97%, SCD41 CO2 about1759–1760ppm and SGP40 index1.
  DTR-disabled capture produced no log lines (USB console behavior, not sensor failure).
* User explicitly authorized ESP32 upload and boot/sensor verification. No RP2040
  flash/write command has been issued.
* First ESP32 upload completed with flash hash verification. A45s boot capture
  showed PSRAM memory test OK, correct Seeed board detection, initialized Wi-Fi
  stack, no panic/watchdog/reset loop, and **real UART** readings23.51°C,
  46.63% RH,1854ppm CO2. VOC initially emitted invalid/warm-up values (withheld).
* User confirmed **physical touch works**, but reported **flickering/shifted lines**.
  The initial80MHz/33ms-panel-scanout build is therefore **not accepted** for
  display stability. The corrective build restores Seeed's120MHz+XIP memory
  profile and10ms panel task, while retaining LVGL's33ms rendering budget.
* A further300s passive ESP32 capture confirmed the **fourth real UART metric**:
  VOC index1 at uptime249.4s. With the existing RP2040 firmware, the gas-index
  algorithm warm-up spans about4minutes because it is called roughly every5.25s.
  The previous rejected frames were withheld warm-up values, not substituted
  healthy measurements. No ESP32 panic/reset loop was observed in this capture.
* Corrective0.1.0 firmware was built through the digest-pinned helper, flashed
  to ESP32 with all3 written-image hashes verified, and captured for30s. Boot
  confirmed120MHz PSRAM, XIP allocation,0.1.0 version and real sensor reception.
  **User then confirmed the display is stable, including page switching.**
  Restoring the official memory/scanout baseline resolved the reported physical
  flicker/shift; this is a functional observation, not a measured FPS/24h soak test.
* Separate latency improvement build passed4/4 normal and4/4 ASan/UBSan suites,
  including press-triggered navigation. It retains the accepted memory/scanout
  settings and changes input polling, event handling, UI priority and removes
  expensive page-opacity fades. ESP32 flash hashes verified,120MHz profile retained.
  Measured touch-I2C read maxima231–254µs, steady-window handler maxima48.4–52.8ms,
  flush maxima20.1–22.8ms; startup maximum handler106.4ms/flush32.8ms. These are
  maximum software durations per10s window, not averages or physical touch latency.
  **User confirmed “Responsive now”: prompt taps/typing and stable display.**

### LoRa extension D1S upload

* User requested SenseCAP first. CH340 `/dev/ttyUSB0` identified; RP2040 and
  XIAO firmware untouched. New private full 8388608-byte backup saved as
  `backups/sensecap-before-lora-20261103-151227.bin` with adjacent SHA256, mode0600.
* Bootloader, partition table and 1684912-byte app flashed successfully; all three
  written-image hashes verified. No NVS erase operation was performed.
* A 45s boot capture confirmed 120MHz PSRAM, XIP and memory test OK, real UART
  temperature20.63°C, RH55.91%, CO21936ppm; no panic/reset loop observed.
  VOC not yet observed during this short capture. Wi-Fi used saved settings but
  association was refused/timed out and retries continued; connectivity not verified.
* Physical display/touch confirmation of the new build, XIAO Wi-Fi provisioning,
  and genuine RF/two-device acceptance remain outstanding.
* Follow-up host-editor/modal correction: user-authorized ESP32 upload of the
  1685776-byte build completed with all three image hashes verified. Current
  24KiB NVS was backed up privately as
  `backups/sensecap-nvs-before-host-modal-20261103-153943.bin` before writing.
  A 30s capture confirmed memory test OK/120MHz, temperature20.78°C, RH56.16%,
  CO22050ppm, successful saved-Wi-Fi association and a DHCP address. No panic/reset
  loop observed. Physical host-editor/tab-flicker confirmation remains pending.

### XIAO receiver upload and initialization

* User confirmed antenna attachment and authorized backup/flash. The XIAO was
  identified by MAC10:b4:1d:e9:14:5c, ESP32-S3 revision0.2, 8MB flash. Its original
  full 8388608-byte image and SHA256 are private/ignored under
  `backups/xiao-before-sniffer-20261103-152306.*`. USB readout was slow; the backup
  completed before any firmware writes.
* Bootloader, partition table and 852288-byte app flashed with all three written
  hashes verified. No explicit NVS erase was performed. Initial monitor/reset
  landed in ROM download mode; user reconnected the XIAO.
* A subsequent 20-second log capture showed `lora_sniffer`0.2.0 booting and
  `RX only: 867500000 Hz SF8 BW125000 CR4/5 sync=12 mode=fixed`, with no panic or
  radio-configuration error. This confirms initialization, **not reception of an
  actual packet**. Sniffer Wi-Fi remained disabled because provisioning is absent.
* SenseCAP was disconnected during this check and remained untouched during
  follow-up XIAO verification.
* User subsequently approved private credential reuse and provisioning. The
  SenseCAP NVS snapshot was CRC-checked and its current settings blob decoded
  locally. New random token and provisioning JSON live only in ignored mode0600
  files under `secrets/`; password/token were not printed. USB acknowledged
  successful NVS commit/reboot, and subsequent boots loaded the settings.
* Live XIAO Wi-Fi/DHCP and hostname resolution observed. Desktop authenticated
  status telemetry (`radio_ready:true`, RX counter0), ping/pong and wrong-token
  error/closure without telemetry passed against the running firmware.
* Live testing found/fixed missing PMF capability/overlapping connection attempts
  and a zero-length-frame HTTPD wrapper bug. The latter treated a zero-length
  payload as another header request, breaking empty FIN continuation/control
  frames. Corrective XIAO uploads passed all three hash checks; portable empty-FIN
  regression and all six normal/sanitizer suites passed.
* AP authentication/temporary-association failures were observed, especially
  around repeated native-USB monitor resets. Long-run Wi-Fi reliability, real RX,
  scan/CAD and authenticated D1S-to-XIAO telemetry remain unqualified.
* Final-image retest was blocked: the latest image booted with radio ready and
  obtained DHCP, but desktop TCP access timed out and its ARP neighbour entry was
  FAILED. Network-only bounded retries also failed. Earlier authenticated status,
  ping and bad-token passes predate the final empty-frame/AP-retry changes; the
  final comprehensive script did not pass. No successful packets were observed,
  and scan/CAD were not confirmed exercised. Default fixed RX was reported on
  startup. Further acceptance needs reliable AP/LAN reachability and a genuine
  matching transmitter; SenseCAP remains disconnected.

## Independently buildable milestones

| Milestone | Present source | Verification boundary |
|---|---|---|
| Foundation | Official BSP, direct LVGL, touch mapping, navigation, Wi-Fi, NVS settings | Cross-build + desktop UI passed; physical touch/page switching/stable display confirmed; NVS persistence/Wi-Fi acceptance remains |
| Climate | Real UART acquisition, calculation, thresholds, charts/history, MQTT publish | All4 real UART metrics confirmed on ESP32; parsing/calculation/UI tests passed; MQTT end-to-end not verified |
| Network | Local state, four background TCP/HTTP host checks, history/details | Cross-build + desktop fixtures; router/server tests needed |
| Reticulum | Standalone authenticated bridge, inactive firmware page, extension contract |6 bridge tests passed; actual installed RNS compatibility not tested; ESP32 client deferred |
| LoRa | Separate XIAO SX1262 RX firmware, D1S WebSocket client/UI, wire schema/tests | Both flashed/boot checked; XIAO RX initialization confirmed; provisioning, packets and two-device test outstanding; see [details](lora.md) |

## Hardware acceptance checklist before a production release

- [x] Back up existing ESP32 flash and verify backup hash/size; obtain upload approval.
- [x] Flash ESP32 only, watch115200-baud boot logs for PSRAM/display errors.
- [x] Physical display and touch functional; page switching/display stability confirmed by user.
- [ ] Exhaustively test touch corners, swipes and long strings/no clipped labels.
- [ ] Read all four real sensors for30min; compare with RP2040 USB output.
- [ ] Remove AHT20; require individual fields/derived values to expire, no fake zeros.
- [ ] Confirm VOC warm-up, CO2 zero rejection, sensor reconnect and stale recovery.
- [ ] Verify1h/6h/24h charts across real elapsed time, plus reboot history reset.
- [ ] Exercise backlight/timeout/wake, keyboard scrolling, host details and thresholds.
- [ ] Save settings, power-cycle, verify NVS persisted without exposing secrets.
- [ ] Configure authorized local Wi-Fi; drop/reconnect AP and DHCP, test bad credentials.
- [ ] Check configured TCP service and HTTP HEAD2xx; unplug host, test DNS/timeout.
- [ ] Publish to a real local MQTT broker; verify LWT, null handling and HA expiry.
- [ ] TLS with valid/invalid certificates and local NTP; never bypass validation.
- [ ] Measure frame time, touch latency, minimum free internal heap, largest block and
  task stack watermarks with Wi-Fi+MQTT+charts+settings concurrently active.
- [ ] Run24h soak, repeated navigation/settings cycles; check no fragmentation trend,
  watchdog resets, display tearing or queue starvation.
- [ ] Audit inherited vendor-driver warnings and signing/encryption/provisioning plan.

RP2040 recovery instructions are provided **before** any future coprocessor
modification; this project requires none. LoRa defaults disabled until an actual
authenticated XIAO is configured; its hardware acceptance remains outstanding.
