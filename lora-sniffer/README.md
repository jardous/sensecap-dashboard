# XIAO ESP32-S3 + Wio-SX1262 passive receiver

This is a **separate ESP-IDF 5.1.6 application**, not a D1S component. The D1S has
no SX1262. RadioLib 7.1.2 and SPI access are compiled only here.

See **[setup, security, modes and verification](../docs/lora.md)** and the
**[schema-1 WebSocket contract](../docs/lora-protocol.md)**.

From the repository root:

```sh
bash scripts/build-sniffer.sh
```

With native IDF 5.1.6, run `idf.py build` from this directory after bootstrap.
Artifacts are in `lora-sniffer/build/`. Build helpers never flash automatically.
Both devices have been flashed and boot-checked, and XIAO SX1262 receive
initialization succeeded. Private USB provisioning and desktop authenticated
WebSocket telemetry/ping/wrong-token rejection have been exercised. Matching the
user's EU868 Long Fast profile produced three real CRC-valid packet events on the
authenticated WebSocket. The user also confirmed XIAO visible and packet data
appearing on SenseCAP; sequence comparison and long-run Wi-Fi reliability remain
outstanding.
The original XIAO 8 MB flash
is privately backed up. Do not overwrite the D1S or RP2040 when selecting a USB port.

## USB connection diagnostics

From the repository root, using the existing pyserial environment:

```sh
.venv-lora/bin/python scripts/sniffer-serial.py status
.venv-lora/bin/python scripts/sniffer-serial.py watch
```

The helper selects the unique Espressif USB device, or accepts `--port PATH`.
The Linux helper changes DTR/RTS together and disables hangup-on-close, avoiding
the reset caused by pyserial's normal sequential modem-line updates. Close other
serial monitors first. `--json` prints structured snapshots. No Wi-Fi/token is
needed; the USB cable must carry data. Ctrl+C stops polling without leaving a
stream active on the device. Unplug/replug and rerun after a USB disconnect.
Four consecutive open/close tests preserved the same running boot/uptime. This
no-reset helper currently supports Linux only and uses pyserial3.5 POSIX hooks.
Other serial monitors may still reset the board; compare counters within one
boot ID. No firmware update is needed for the host-side no-reset correction.

For a serial terminal at115200 with local echo **off**, send LF/CRLF-terminated
`help`, `status`, `wifi`, `watch on`, or `watch off`. `watch on` streams at1Hz for
60 seconds; repeat it to renew. Diagnostics are read-only, bounded and run below
the acquisition task. The pre-existing private provisioning JSON remains the
only USB write operation; no raw radio, transmit or general shell commands exist.

Wi-Fi snapshots distinguish association from a usable DHCP address and show live
RSSI/channel/BSSID, device MAC, gateway/DNS, power-save mode (0=none,1=min-modem,
2=max-modem), attempt/disconnect/timeout counters, retry timing and the eight most
recent disconnect reason codes/names. A disconnect-event RSSI of0 can mean the
driver had no measurement. Passwords/tokens are never included; SSID and local
network addresses **are** included, so treat captures as local diagnostic data.
Listener readiness and an assigned IP are not proof of LAN reachability. The
WebSocket counters distinguish authenticated sessions from rejected credentials.
`status` also returns radio readiness/profile and receive counters, not payloads.
Boot ID/uptime/reset reason and minimum console/event-task stack headroom make
resource-related restarts visible. The accepted hardware test obtained93 Wi-Fi
snapshots with one boot and no runtime reset; this is not a long-term Wi-Fi test.
