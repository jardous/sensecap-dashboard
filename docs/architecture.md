# Architecture

```text
stock RP2040 (AHT20 / SCD41 / SGP40)
  UART2 / PacketSerial COBS
      ↓
sensor RX task → bounded Sample queue (32)
      ↓
app_main (single model + LVGL owner)
  validated readings → freshness / derived measurements → minute-ring history
      ↓                          ↓
  shared LVGL widgets       single-slot telemetry queue
                                  ↓
                         network service → MQTT enqueue

Wi-Fi events → critical-section NetworkState ← host-check worker
                             ↓ copied snapshot
                         app_main / Network UI

settings callbacks → validated draft → single-slot settings queue
                                          ↓
                              service task / atomic NVS blob / reboot
```

## Source boundaries

| Directory | Responsibility |
|---|---|
| `main/app` | Lifecycle, queue draining, render cadence, publication |
| `main/core` | Portable typed model, psychrometrics, COBS parser, validation, history |
| `main/display` | LVGL/BSP bridge, touch and double framebuffer dirty-copy logic |
| `main/ui` | Cards, charts, nav/swipes, detail overlays, settings form |
| `main/sensors` | UART driver and acquisition task; never calls LVGL |
| `main/network` | Wi-Fi events/reconnect, IP/DNS, MQTT, time, snapshots |
| `main/monitor` | IPv4 DNS/TCP and certificate-verified HTTP HEAD checks |
| `main/config` | Validated NVS settings and bounded UART0 USB link setup; never erases unknown NVS |
| `main/reticulum` | Deferred optional bridge client |
| `main/lora` | D1S authenticated WebSocket worker and bounded event/config queues; no radio |
| `shared/lora` | Portable wire schema, state/history and conservative classifiers |
| `lora-sniffer` | Independent XIAO ESP-IDF app; SX1262 RX task, guarded HAL, authenticated server |
| `bridge` | Independent read-only Linux status service and tests |
| `desktop` | Same C++ model/UI; SDL framebuffer + explicit synthetic fixtures |

The current `core/model` includes the in-memory storage primitive; there is no
extra task or filesystem abstraction pretending history is durable. A later
`storage` component can consume completed buckets for SD/flash durability without
changing acquisition/UI ownership.

## Ownership and memory

* Only `app_main` mutates readings/history or calls LVGL. The UI task runs at
  priority5 with a2ms cooperative delay, above background network/host workers.
  `lv_tick_inc` is called
  from a periodic ESP timer as LVGL permits. BSP retains its own LCD synchronization
  task; its flush-completion callback runs through the upstream path.
* Sensor UART task uses a fixed 32-byte frame buffer and 128-byte read buffer;
  queue capacity 32, enqueue with no wait; overflows are logged rather than
  allocating or blocking acquisition. Freshness recovers with later samples.
* `NetworkState` copies and host updates are protected by short FreeRTOS SMP
  critical sections. No sockets, allocations, logging or LVGL calls inside them.
* Wi-Fi event callback doesn't sleep/retry recursively. Service task retries with
  exponential backoff to 32s. Settings are immutable in workers until reboot.
* MQTT runs on IDF's own client task. App queues compact reading snapshots, not
  synchronous network calls. Disconnected state data is dropped, not accumulated;
  MQTT outbox capped at 4096 bytes, state QoS0/nonretained, availability QoS1/retained.
* Host checks run in a separate task, sequentially (four configured slots),
  nonblocking TCP socket + `select`, or bounded-timeout HTTP in that worker.
  DNS has lwIP timeouts. UI never waits for monitoring.
* History has 1440 tagged minute buckets. Four independent sums/counts prevent
  missing sensors from biasing averages. Fixed allocation (~57.6 KiB) and fixed
  60/24-point charts; age-based projection preserves correct empty space after
  boot and handles 24-hour rollover. Monotonic 64-bit timestamps avoid wrap and
  NTP discontinuities. No invented old history after a reboot.
* GUI objects are allocated only at navigation/overlay creation, not continuously
  for measurements. Labels update only on changed text; chart arrays are reused.
  Page switches destroy the previous page, so the working set is bounded.
  Navigation reacts on press, skips redundant current-page rebuilds and does not
  use full-screen opacity fades. Bounded arc/bar animations remain enabled.
* Touch polling is10ms. Press/release transitions schedule a display refresh
  promptly; rendering is never called recursively inside an input callback.
  Physical scanout settings are independent and retain the stable Seeed baseline.
  First-minute `ui_timing` logs report measured maximum handler, flush and I2C-read
  durations without logging touch coordinates, key text or credentials. Those
  timings are **not** a measurement of physical touch-to-photon latency.
* BSP framebuffers are in PSRAM. Main model/history lives in static storage.
  Settings draft/field descriptors are fixed-size. No C++ exceptions/RTTI needed.

## Semantics

Temperature/RH are never substituted with SCD41 internal ambient estimates. Missing
data is `NaN` internally, `--` in LVGL and JSON `null` in MQTT. Expired readings
are not eligible for derived values or an unqualified healthy air badge. Critical
available readings still raise an alarm even if another air sensor is missing.
Comfort and air quality are separate concepts. Values are informative only.

Network online means the configured service responded, not an inferred CPU state.
DNS lookup is included in observed latency. Interface clients are not RNS peers;
RNS destinations are not LXMF peers. There is no universal raw-LoRa decoder.

## External modules

Reticulum consumes the bridge's schema-1 allowlist over authenticated HTTPS in a
dedicated worker; null fields remain unavailable. LXMF needs a separate explicit
source and authorization; this Reticulum client remains deferred. LoRa now uses
the separate XIAO receiver and bounded D1S WebSocket client, portable schema/state
and Probable/Unknown payload classifiers. The radio never runs on D1S. See
[LoRa architecture, ownership, security and test boundary](lora.md).

Remote telemetry is likewise deferred: future adapters must carry source,
timestamp, units, schema and freshness; cap packet sizes and never reinterpret
missing CPU/memory/disk metrics as zero. Do not enable arbitrary remote execution
or administrative APIs to acquire those metrics.
