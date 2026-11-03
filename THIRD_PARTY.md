# Third-party code

`vendor.lock.json` records exact official source revisions. Bootstrap downloads
them without modification; preserve their license notices when distributing.

* Seeed SenseCAP Indicator ESP32 repository: original component licenses in each
  directory (BSP/display adaptation Apache-2.0; LVGL MIT; transitive radio code
  retains its own terms). Review all selected component terms before distribution.
* Seeed SenseCAP Indicator RP2040 repository: inspected for protocol/hardware,
  not compiled/flashed/bundled into this application's binary. Original license
  and dependency terms remain in that checkout.
* `main/display/port.c` adapts Seeed's official LVGL port, itself bearing Espressif
  Apache-2.0 provenance; it is Apache-2.0 rather than relicensed under project MIT.
  Copyright 2015–2022 Espressif Systems (Shanghai) CO LTD.
* ESP-IDF5.1.6 and its dependencies retain their upstream licenses.
* SDL2 is linked only in the desktop simulator, under SDL's upstream license.
* RadioLib7.1.2 (MIT) is linked only into the separate XIAO receiver firmware.
* Espressif esp-protocols (Apache-2.0), pinned at websocket-v1.2.3: WebSocket client
  on D1S and mDNS on XIAO. ESP-IDF supplies the HTTP/WebSocket server.
* cJSON1.7.15 (MIT) is pinned for desktop protocol tests/simulation. Firmware uses
  ESP-IDF's bundled JSON component; parser interoperability is hardware-unverified.
* The bridge calls an independently installed `rnstatus` command. No Reticulum
  source is copied or bundled. Reticulum has its own license and must be installed
  and used in compliance with it. This project's license does not supersede it.

Font assets are LVGL's bundled Montserrat fonts. No web fonts, cloud images,
external rendering service or generated image asset is needed at runtime.
