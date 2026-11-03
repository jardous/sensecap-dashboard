# Backup, flashing and recovery — read before writing firmware

The application changes **ESP32 firmware only**. It does not update RP2040, its
bootloader, sensor firmware, SD contents, calibration or eFuses. Confirm a stable
USB data cable/power supply before starting; do not unplug while writing flash.

## 1. Identify ports

On the connected development unit Linux enumerates an internal USB hub with:

* CH340 (`1a86:7523`) → `/dev/ttyUSB0`, **ESP32** programming UART.
* Seeed Indicator RP2040 (`2886:0050`) → `/dev/ttyACM0`, **RP2040** USB serial.

Names can change on reconnect. Prefer `/dev/serial/by-id/` and check `lsusb`.
Do not send ESP32 flashing commands to the RP2040 port. A power-only cable can
illuminate the screen without exposing any USB device. Check the official
[port/button illustrations](https://wiki.seeedstudio.com/SenseCAP_Indicator_How_To_Flash_The_Default_Firmware/)
instead of guessing which internal button to press.

Native IDF environment provides `esptool.py` (tested 4.7.0). Run:

```sh
esptool.py --chip esp32s3 --port /dev/ttyUSB0 flash_id
```

This resets the ESP32 and uploads a volatile RAM stub; it does **not write flash**.
The RP2040 USB port can briefly disappear during the board reset.

## 2. Save the entire original flash

First confirm the detected size is **8 MB**. Protect the backup: it may contain
Wi-Fi passwords, MQTT tokens or other factory/user secrets. Never commit it.

```sh
umask 077
mkdir -p backups
esptool.py --chip esp32s3 --port /dev/ttyUSB0 --baud 460800 \
  read_flash 0x0 0x800000 backups/esp32-original.bin
chmod 600 backups/esp32-original.bin
sha256sum backups/esp32-original.bin > backups/esp32-original.sha256
```

Use a new filename rather than overwriting an older backup. Confirm exactly
8388608 bytes, and keep a second secure copy. For a stronger backup check, read
the flash twice and compare hashes before permitting a firmware write.

Docker example (uses the pinned image from `vendor.lock.json`):

```sh
IMAGE=$(python3 -c 'import json; print(json.load(open("vendor.lock.json"))["idf_image"])')
docker run --rm --user "$(id -u):$(id -g)" \
  --group-add "$(stat -c %g /dev/ttyUSB0)" --device=/dev/ttyUSB0 \
  -v "$PWD:/project" -w /project "$IMAGE" \
  esptool.py --chip esp32s3 --port /dev/ttyUSB0 --baud 460800 \
  read_flash 0x0 0x800000 backups/esp32-original.bin
```

Adding the serial device's group to the container is required even if the host
user belongs to `uucp`/`dialout`. This needs no host permission changes or
`--privileged` container. Do not run unrelated serial monitors concurrently.

## 3. Flash this application (explicit approval required)

Build and tests first. **Flashing replaces the current ESP32 application and
partition table.** It can make old application-specific settings inaccessible;
the full backup is the recovery path. Existing bytes outside the written ranges
are not proactively erased, but should not be relied on as a backup.

Native IDF:

```sh
idf.py -p /dev/ttyUSB0 flash monitor
```

Docker:

```sh
docker run --rm --user "$(id -u):$(id -g)" \
  --group-add "$(stat -c %g /dev/ttyUSB0)" --device=/dev/ttyUSB0 \
  -e HOME=/tmp -v "$PWD:/project" -w /project "$IMAGE" \
  idf.py -p /dev/ttyUSB0 flash
```

Use the generated `build/flash_args` / `flasher_args.json`. Project offsets:

| Artifact | Offset |
|---|---|
| `build/bootloader/bootloader.bin` | 0x0000 |
| `build/partition_table/partition-table.bin` | 0x8000 |
| `build/sensecap_dashboard.bin` | 0x10000 |

Some old wiki examples show another project's partition-table offset. **Do not
copy it**; this project's generated offsets are authoritative. Do not write the
standalone application binary at zero. Factory app partition is 0x3F0000 bytes;
no OTA slots, signing or remote update endpoint in v0.1.

Monitor at **115200 baud**. Wait at least 20–60 seconds for sensor startup and
VOC warm-up. Check real values, frame rejects, missing sensors, screen corners,
brightness and swipes. Configure Wi-Fi locally and verify reconnect/persistence.
No sensor values should be synthesized when USB/RP2040/AHT20 is missing.

If the display is blank but logs show the app runs, check the **GX vs DX panel**
selection and RGB/PSRAM initialization. If flashing auto-reset fails, consult
Seeed's ESP32 boot-mode/button instructions for the exact board revision. Do not
press/short unknown pins or repeatedly erase flash hoping to repair a cable.

## 4. ESP32 recovery

Restore the full original image **only to the same device**, with its detected
flash size, after confirming the backup hash:

```sh
sha256sum -c backups/esp32-original.sha256
esptool.py --chip esp32s3 --port /dev/ttyUSB0 --baud 460800 \
  write_flash 0x0 backups/esp32-original.bin
```

This restores firmware, partition table and NVS from the backup. It overwrites
all new settings. Secure-boot/flash-encryption devices require their original
provisioning/recovery procedure; don't assume raw backup restoration is valid
across devices. No irreversible security eFuse operation is part of this project.

Alternatively get the correct factory ESP32 image from the official
[Seeed releases](https://github.com/Seeed-Solution/SenseCAP_Indicator_ESP32/releases)
and follow its version-specific flashing instructions. Factory images may not
restore your user configuration; backups do.

If NVS reports no free pages/version mismatch, the app logs the failure and
does not automatically erase user data. Only after a full backup and explicit
authorization may the application's NVS range (0x9000, length 0x6000 under **this
partition table**) be erased. Do not use whole-flash erase as routine recovery.

## 5. RP2040 recovery BEFORE any future coprocessor changes

**Not required for this application. Leave the working RP2040 alone.** If a later
sensor-firmware project is attempted:

1. Download/keep a known-good official UF2 for this board and review Seeed's
   [RP2040 recovery instructions and BOOTSEL button photograph](https://wiki.seeedstudio.com/SenseCAP_Indicator_How_To_Flash_The_Default_Firmware/#flash-the-uf2-file).
2. Record firmware and library versions, sensor calibration/settings, and save SD
   logs separately. An ESP32 flash backup does **not** back up RP2040 flash.
3. Enter RP2040 BOOTSEL by holding the **documented RP2040 internal button** while
   connecting its USB data port, then releasing it. A boot volume such as
   `RPI-RP2` appears. This is not the ESP32 UART boot mode.
4. Copy the **Seeed Indicator RP2040** known-good `.uf2` to that volume; it ejects
   and reboots. Never copy an ESP32 `.bin` there or an unrelated Pico build.
5. Reconnect, verify sensor values on RP2040 USB serial at 115200 and verify UART
   packets reach the ESP32. Only then consider custom firmware development.

No RP2040 write or recovery operation has been performed by this project.
