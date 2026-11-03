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

## Network host editor and modal input isolation

The user subsequently reported host long-press opening the wrong configuration
window and background tab buttons flickering while an overlay was visible.

Two UI causes were identified independently of RGB scanout/timing:

* Every host's long-press callback opened the general settings form without a
  host index. It now opens a dedicated four-field editor for the selected slot;
  Save preserves all other settings. Short-click opens host details/Edit, or
  directly edits an empty slot. `LV_EVENT_SHORT_CLICKED` separates this from the
  click-on-release emitted after a long press.
* Overlays previously covered only their own rectangle. `navigate()` refused
  page changes while a modal existed, but exposed tabs could still receive input
  and display their pressed style. All overlays now share a full-screen,
  transparent clickable shield. It blocks input/gesture propagation without
  adding dimming/fades or changing the accepted display/memory settings. The
  keyboard is parented above the modal within that shield. Opening/closing waits
  for the current pointer release to prevent click-through.

Desktop regression tests drive the actual LVGL pointer: long-press all four
hosts, check the selected editor's values, cancel without mutation, save only
one slot, exercise short-click/Edit, and press a background tab while a modal is
open. They require no background pressed state/navigation and identical tab
framebuffer pixels before/during the blocked press. All six normal and six
ASan/UBSan suites passed, and the corrective firmware was uploaded with all three
image hashes verified. Current NVS was privately snapshotted before upload; a
30-second boot capture confirmed 120MHz PSRAM, memory test OK, real sensor input
and successful local Wi-Fi/DHCP. Physical confirmation of the two UI fixes is
still pending user interaction.

## Intermittent window flashing after USB pairing

User describes irregular window flashing like a stuck input/pressed key; there
is no clear idle/tab/settings correlation yet. This is not established as serial
interference or a recurrence of the original scanout problem. Source/pin review:

* XIAO native USB Serial/JTAG is a separate chip/device from the SenseCAP.
* SenseCAP's setup listener uses UART0 (default console GPIO43/44); RP2040 data
  uses UART2 GPIO19/20; neither overlaps the D1S RGB panel pins.
* LVGL registers only a touch pointer here, not a USB/serial keyboard. UART setup
  cannot inject key/touch/navigation events. The listener never calls LVGL.
* The touch adapter initializes each sample to released; the BSP also has an
  error-triggered touch reset. No proof of a ghost/stuck touch or its cause was
  obtained from source inspection. The existing full-screen modal input shield
  and release suppression remain in place.
* The CRC-checked NVS snapshot made after USB pairing has page rotation0 (off).
  This is the saved snapshot, not a live observation of subsequent UI edits.

Do not claim a fix or change accepted120MHz PSRAM/XIP, panel/input10ms, render33ms,
UI priority5 or panel porch timings on this evidence. Next useful measurement is
privacy-safe touch transition/held-duration/read-error counters correlated with
navigation events, plus checking auto-page rotation and power/reset events while
the symptom is reproduced. No SenseCAP reflash was performed for XIAO diagnostics.

Follow-up: the user correlated rapid page switching with host test runs and
explicitly identified the **physical SenseCAP**, not desktop windows. The current
simulator's `--self-test` returns before SDL window creation and opens no hardware
serial endpoint; no interactive simulator was running when checked. This does
not explain or disprove the user's correlation. Extra page-change tracing was
proposed but not included when the user requested the button-only blanking upload.
That upload disables idle blanking and implements GPIO38 button blank/wake; it
does not claim a fix for intermittent page switching. Panel timing is unchanged.
