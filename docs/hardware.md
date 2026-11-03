# Verified upstream hardware/software findings

Inspected the actual source before implementing the application. Revisions are
fixed in `vendor.lock.json`; links below refer to official repositories.

## Compatibility

* ESP32 README explicitly requires **ESP-IDF 5.1.x**, not older/newer major/minor
  versions. Its basis example defaults were generated with **5.1.1**.
* Selected/test-built **5.1.6** using Espressif's digest-pinned container, GCC 12.2.
* Bundled `components/lvgl/lvgl.h` declares **8.3.1**. No LVGL 9 APIs are used.
* `CONFIG_LCD_BOARD_SENSECAP_INDICATOR_D1` is the official **D1/D1S** profile;
  D1L/D1Pro is the separate SX1262-enabled profile. We never select it.
* Physical USB probe on this unit: ESP32-S3 QFN56 rev 0.2, 40 MHz crystal,
  embedded **8 MB PSRAM**, **8 MB** external flash, quad flash eFuse mode.

## Display and touch — reuse BSP

Sources under `vendor/seeed-esp32/`:

* `components/bsp/src/boards/sensecap_indicator_board.c`
* `components/bsp/src/peripherals/bsp_lcd.c`
* `components/bsp/src/boards/lcd_panel_config.c` (`lcd_panel_st7701s_init`)
* `components/bsp/src/indev/indev_tp.c`
* `components/i2c_devices/touch_panel/ft5x06.c`
* `examples/indicator_basis/main/lv_port.c`

480×480, RGB565, 16-bit parallel RGB. Default **GX** panel uses ST7701S serial
initialization through the expander; **DX** is a distinct RGB-only panel option
in `idf.py menuconfig` → Board Configuration. Do not blindly replace the panel
sequence if your device has another display revision.

| Function | ESP32 GPIO / details |
|---|---|
| VSYNC / HSYNC / DE / PCLK | 17 / 16 / 18 / 21 |
| Backlight | 45, active high; application adds 5 kHz, 10-bit LEDC dimming |
| B0–B4 | 15,14,13,12,11 |
| G0–G5 | 10,9,8,7,6,5 |
| R0–R4 | 4,3,2,1,0 |
| I2C SDA/SCL | 39/40 |
| I/O expander | 0x20, detection fallback 0x39 |
| Expander touch reset | bit 7 |
| Expander LCD CS/reset | bits 4/5 |
| Expander RP2040 reset | bit 8, released high at board initialization |
| User button | 38 |

RGB clock 18 MHz, horizontal porch back/front/pulse **50/10/8**, vertical
**20/10/8**, `pclk_active_neg=0`. GX uses the FT-family driver at **0x48**
(upstream comment says FT6336U), DX at **0x38**. The requested generic FT5X06
family is supported by the original driver; do not assume every revision is at
0x38. Rotation-0 touch inversion follows the basis example, clamped to 0–479.
Physical corner alignment remains an acceptance test.

Display port retains two BSP RGB framebuffers (2×480×480×2 = **921600 bytes** in
PSRAM), VSYNC synchronization and dirty-area copy/cache writeback in direct mode.
LVGL rendering uses a33ms (~30FPS) budget. The panel scanout task retains
Seeed's **10ms** scheduling period at18MHz PCLK; the two timings must not be
conflated. No full-screen animation loop. Graphics rendering is owned by `app_main`.

The official example uses experimental **120MHz flash/PSRAM** and
instruction/rodata fetch from PSRAM. This project now retains that **Seeed board
baseline**, including octal PSRAM and XIP/rodata relocation. An initial80MHz build
with33ms panel scanout booted and acquired sensors, but the user reported physical
flickering/shifted lines. That configuration is not accepted as a stable default.
Restoring the official baseline is a targeted display fix, not a new driver.
The120MHz profile is explicitly experimental in IDF; qualify the physical unit
under Wi-Fi/TLS/NVS load. Do not claim all memory/panel revisions support it.

IDF5.1.6 documents RGB PSRAM/flash bandwidth sharing and resulting display
corruption at insufficient bandwidth. Bounce buffers are a possible future
fallback, but are **incompatible with this BSP's refresh-on-demand mode** and must
not simply be added to its config without redesigning its synchronization.

## Sensor coprocessor — retain firmware

Sources:

* ESP32: `examples/indicator_basis/main/model/indicator_sensor.c`
* RP2040: `examples/indicator_rp2040/indicator_rp2040.ino` (declares v1.0.1)

ESP32 **UART2 TX19/RX20**, 115200 baud, 8N1, no flow control; RP2040
Serial1 **TX16/RX17**. Sensor I2C on RP2040 **SDA20/SCL21**, power GPIO18;
SCD41 address 0x62. Grove AHT20 readings are sent only on a successful read;
SGP40 uses the Sensirion VOC algorithm and AHT-derived compensation. CO2 packets
with zero are rejected by this application (upstream can still transmit them).

Each decoded packet is exactly **5 bytes**: `type`, then **IEEE-754 binary32,
little-endian**. PacketSerial COBS encoding, `0x00` delimiter. No checksum or
explicit validity flag exists. The bounded parser supports arbitrary UART read
fragmentation, multiple packets, overflow/discard-until-delimiter, malformed COBS,
non-finite/range rejection and re-synchronization. It cannot detect every plausible
in-range bit corruption in a protocol without a CRC.

| Type | Meaning |
|---|---|
| 0xB2 | SCD41 CO2 ppm |
| 0xB3 | **AHT20** temperature °C (historically mislabeled SHT41 upstream) |
| 0xB4 | **AHT20** relative humidity % |
| 0xB5 | SGP40 VOC index, 1–500; zero is warm-up/unavailable |

0xB0/0xB1 SCD temperature/humidity are intentionally not used as AHT20 fallback.
Failures age independently per metric. Calculated humidity values require both
fresh temperature and RH. No firmware command is sent to the RP2040.

RP2040 README library versions: PacketSerial 1.4.0, Sensirion Core 0.6.0,
SGP40 0.1.0, SCD4x 1.1.0, Gas Index Algorithm 3.2.2, AHT20 1.0.1.
The current sketch's SCD API call signature may require a different library
revision than this old table. **No RP2040 rebuild has been claimed or attempted**;
retain the known-working firmware/official UF2 and verify APIs before any later
coprocessor work. The stock sketch's approximate 5.1s VOC processing cadence is
an upstream limitation; do not claim a newly validated Sensirion algorithm cadence.

microSD is owned by RP2040 SPI1 pins 10/11/12, CS13, not ESP32 SDMMC. Its sketch
may log CSV itself. This project neither mounts nor writes the SD card.

## Vendor warning boundary

The original SDK produces legacy ADC/I2S/RMT deprecations, log format mismatches,
unused-driver warnings and some incompatible-pointer diagnostics. GCC 12 upgrades
some to errors; our CMake downgrades **format** and **maybe-uninitialized** to
warnings only on `bsp`, `bus`, `i2c_devices`, `iot_button`, without modifying
their source. Non-FT touch drivers are compiled by vendor CMake but not selected
on D1S. These warnings are not a claim that the SDK is production-audited; an
upstream driver audit is required before deployment certification. LoRa sources
are a transitive BSP build dependency, but the D1/D1S branch never initializes them.
