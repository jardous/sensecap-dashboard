# Verification and release status

Development snapshot: **2026-11-03**, v0.1.0. No production qualification claimed.

## Commands executed successfully

* Inspected/cloned both official Seeed repositories at `vendor.lock.json` revisions.
* Firmware build in `espressif/idf:v5.1.6`, GCC12.2, bundled LVGL8.3.1:
  `idf.py build`; bootloader, partition table, ELF and application BIN produced.
  Latest recorded app size **1624768 bytes**, fits 0x3f0000 partition
  with **61% free**. Final generated artifacts are authoritative if rebuilt.
* Initial `idf.py size` measurement: static D/IRAM about188KiB, about158KiB left
  before runtime heap allocations (not a measured runtime free-heap guarantee).
  This predates the XIP corrective build; do not use it as the final runtime
  internal-heap minimum. RGB buffers are in PSRAM.
* Compiler command checked: final option `-std=gnu++17`, overriding IDF's default.
* Desktop Debug CMake build with real bundled LVGL and SDL2.
* `ctest --test-dir build-desktop --output-on-failure`: **4/4 suites passed**.
* AddressSanitizer + UndefinedBehaviorSanitizer Debug build:
  `ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-sanitize --output-on-failure`:
  **4/4 suites passed**. Process-lifetime LVGL allocation leaks were not assessed.
* `python3 -m unittest discover -s bridge/tests -v`: **6 tests passed**.
* Headless shared-LVGL previews generated in `docs/previews/`, including real
  unavailable states. Climate/Network previews use clearly labeled synthetic
  desktop fixtures; optional-page previews contain no pretend live data.

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
   keyboard focus/close and missing/stale paths, both simulated and unavailable.

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

## Independently buildable milestones

| Milestone | Present source | Verification boundary |
|---|---|---|
| Foundation | Official BSP, direct LVGL, touch mapping, navigation, Wi-Fi, NVS settings | Cross-build + desktop UI passed; physical touch/page switching/stable display confirmed; NVS persistence/Wi-Fi acceptance remains |
| Climate | Real UART acquisition, calculation, thresholds, charts/history, MQTT publish | All4 real UART metrics confirmed on ESP32; parsing/calculation/UI tests passed; MQTT end-to-end not verified |
| Network | Local state, four background TCP/HTTP host checks, history/details | Cross-build + desktop fixtures; router/server tests needed |
| Reticulum | Standalone authenticated bridge, inactive firmware page, extension contract |6 bridge tests passed; actual installed RNS compatibility not tested; ESP32 client deferred |
| LoRa | Inactive firmware page, adapter contract, no radio assumptions | No client/gateway implemented; genuine source required |

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
modification; this project requires none. Optional external integrations remain
disabled until actual authenticated sources/adapters and tests exist.
