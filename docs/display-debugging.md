# Display flicker investigation — 2026-11-03

## Observed failure

The initial firmware successfully booted, initialized the board, passed the80MHz
PSRAM test and received genuine sensor data. The user confirmed touch worked but
reported **flickering or shifted lines**, rather than merely choppy animations.
No panic/watchdog/reboot loop appeared in the serial captures.

Compared the application's display/memory settings against the pinned official
`examples/indicator_basis/sdkconfig.defaults` and IDF5.1.6 RGB LCD documentation.
The initial application had two consequential deviations:

*80MHz flash/PSRAM without the official instruction/rodata relocation.
*33ms **panel** scanout task period, conflating panel refresh with LVGL rendering.

RGB scanout bandwidth is shared with CPU accesses to external memory. IDF's
[RGB LCD documentation](https://docs.espressif.com/projects/esp-idf/en/v5.1.6/esp32s3/api-reference/peripherals/lcd.html#rgb-interfaced-lcd)
describes starvation/corrupted or shifted output, and the role of PSRAM/XIP.
This is distinct from LVGL's ability to animate at30FPS.

## Accepted correction

Restored the **existing official board baseline**, without replacing the BSP:

```text
CONFIG_IDF_EXPERIMENTAL_FEATURES=y
CONFIG_SPIRAM_SPEED_120M=y
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
CONFIG_ESPTOOLPY_FLASHFREQ_120M=y
CONFIG_LCD_TASK_REFRESH_TIME=10
CONFIG_LV_DISP_DEF_REFR_PERIOD=33
```

PCLK18MHz, panel porch/init sequence, double PSRAM buffers, direct-mode dirty copy
and touchscreen driver were unchanged. The application still avoids unnecessary
full-screen redraws. IDF's generated flash image header/flashing arguments can say
80MHz despite selecting experimental120MHz; runtime tuning/logs and the generated
Kconfig are authoritative for the actual memory mode.

Used a separate `build-display` configuration for comparison, then regenerated
the development-generated root `sdkconfig` and rebuilt canonical `build/` from
the checked-in defaults. Existing user configuration was not discarded; it was
the agent-created build configuration. Both paths built successfully. Final
canonical0.1.0 BIN size1624384 bytes (0x18c940),61% app-partition headroom.

After the corrective ESP32-only flash, boot logs confirmed120MHz PSRAM and normal
sensor reception. **User explicitly reported “Stable now” for the Climate screen
and page switching.** Physical touch was already confirmed. This establishes a
working configuration for this unit, not independent proof of the precise
starvation mechanism: several official baseline settings were restored together.

## Reproduction / future changes

Do not rely on changing `sdkconfig.defaults` alone to override an already-generated
`sdkconfig`. For a clean comparison that preserves existing configuration:

```sh
idf.py -B build-display -D SDKCONFIG="$PWD/build-display/sdkconfig" build
```

`config/display-seeeds-baseline.defaults` records the recovery overlay. Current
main defaults already include it. Keep a saved known-working binary and the full
original firmware backup before more display experiments.

Do not blindly enable bounce buffers: IDF5.1.6 explicitly rejects them together
with the BSP's `refresh_on_demand` flag. That alternative requires a proper
continuous-scanout and framebuffer-synchronization design, not a one-line tweak.

Still required before production:24h soak, Wi-Fi/TLS/NVS load, corner-touch tests,
measured frame times, heap fragmentation and task stack watermarks.120MHz remains
an experimental IDF option inherited from Seeed; qualify each board/memory revision.

## Separate touch-latency investigation

After confirming stable scanout the user reported slow tapping/typing and bottom
navigation. These are tracked separately from display corruption. The follow-up
build changes only UI scheduling/interaction, **not the working panel timings**:

*10ms input timer instead of30ms (explicit runtime setting plus defaults).
*UI task priority5 instead of default main-task priority1;2ms rather than5ms yield.
*Navigation on `LV_EVENT_PRESSED` rather than waiting for release.
*No150ms full-screen opacity fades, which caused extra480×422 compositing/redraws.
*No rebuilding the current page when its navigation button is pressed again.
*Input transitions mark the display timer ready; no recursive render in callbacks.
*First-minute timing counters record worst LVGL handler, BSP flush and touch-I2C
  duration, never touch coordinates, password text or keyboard events.

The desktop smoke tests cover the actual press event as well as direct page API
navigation, settings and keyboard lifecycle. Physical latency must still be
confirmed by user interaction; timer configuration alone is not a latency result.

**Outcome:** follow-up upload passed all3 flash hash checks and booted normally.
Measured touch reads231–254µs, steady-window LVGL handler maxima48.4–52.8ms and
flush maxima20.1–22.8ms (initial startup peak106.4ms handler). User then explicitly
confirmed **“Responsive now”**, including prompt taps/typing and stable display.
These measurements do not constitute a physical touch-to-photon benchmark.
