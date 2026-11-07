# Espressif32 8 / HomeSpan 2 upgrade

The build failure after removing the Espressif32 6.13.0 pin is reproducible.
HomeSpan 1.9.1 explicitly requires Arduino-ESP32 2.x, while Espressif32 8.0.0
ships Arduino-ESP32 3.3.12. Updating HomeSpan resolves that incompatibility,
but the resulting application also needs more flash space than the old default
application slots provide.

The current configuration pins **Espressif32 8.0.0** and **HomeSpan 2.1.8** and
selects the standard **min_spiffs.csv** layout for both supported 4 MB boards.
No FujiHK or FujiHeatPump source changes were required. The ESP32-WROOM-32E
is still a supported compile target; these failures do not establish a hardware
limitation or require a replacement MCU.

## Observed build results

Validation used Linux, Python 3.14, PlatformIO Core 6.1.19, and upstream
FujiHeatPump revision `72e00ec`. The released HomeSpan 2.1.8 tag resolves to
`107ffc0`. The new toolchain uses Arduino-ESP32 3.3.12, ESP-IDF 5.5.5, and
Xtensa GCC 14.2.0 (`20260121`).

| Platform / Arduino / HomeSpan | Partition layout | Board | Result |
| --- | --- | --- | --- |
| 6.13.0 / 2.0.17 / 1.9.1 | default | Both | Pass; XC3800 application 1,234,637 bytes, M5Atom 1,242,709 bytes |
| 8.0.0 / 3.3.12 / 1.9.1 | default | XC3800 | Compile failure: HomeSpan's version guard rejects Arduino 3 |
| 8.0.0 / 3.3.12 / 2.1.8 | default | XC3800 | Links, then fails size check: 1,642,008 bytes exceeds 1,310,720 bytes |
| 8.0.0 / 3.3.12 / 2.1.8 | min_spiffs | XC3800 | Pass; 1,642,008 bytes flash, 62,036 bytes RAM |
| 8.0.0 / 3.3.12 / 2.1.8 | min_spiffs | M5Atom | Pass; 1,658,801 bytes flash, 62,724 bytes RAM |

The first failing configuration reports:

```text
HOMESPAN REQUIRES VERSION 2 OF THE ARDUINO ESP32 LIBRARY.
HOMESPAN IS NOT COMPATIBLE WITH VERSION 1 OR VERSION 3
```

The old HomeSpan `Network.h` also clashes with the new Arduino Network library,
causing additional networking compilation errors. Removing the version guard
would not make that library compatible. Updating to HomeSpan 2.1.8 addresses
the build errors without local patches to either dependency.

The newer compiler also reports three warnings in the unchanged FujiHK source:
two redundant non-negative comparisons of unsigned byte values, and a `%X`
format/type mismatch in the serial-number formatting. They do not fail the
build. Source cleanup is outside this configuration upgrade.

Rebuild the current configuration from the repository root using the Python
environment installed in [building.md](building.md):

```sh
.venv/bin/pio run -e esp32dev -e m5stack-atom
```

To reproduce the failing combinations without editing the tracked configuration,
create temporary copies of `platformio.ini`: remove `board_build.partitions`
for the size failure, and additionally change `#2.1.8` to `#release-1.9.1` for
the compatibility failure. Run `.venv/bin/pio run --project-conf <temporary-file> -e esp32dev`
from the repository root. A normal build restores the configured dependencies.

## Partition layout and first upgrade

Both layouts fit within the board profiles' 4 MB flash. The new layout retains
two application slots; it does not enable OTA updates in FujiHK.

| Partition | Previous offset / size | New offset / size |
| --- | --- | --- |
| NVS | `0x9000` / `0x5000` | Unchanged |
| OTA metadata | `0xe000` / `0x2000` | Unchanged |
| app0 | `0x10000` / `0x140000` | `0x10000` / `0x1e0000` |
| app1 | `0x150000` / `0x140000` | `0x1f0000` / `0x1e0000` |
| SPIFFS | `0x290000` / `0x160000` | `0x3d0000` / `0x20000` |
| Core dump | `0x3f0000` / `0x10000` | Unchanged |

Each application slot increases from 1,310,720 to 1,966,080 bytes. This reduces
the filesystem partition to 128 KiB; FujiHK does not use SPIFFS in its source.
NVS holds HomeSpan's Wi-Fi and pairing data, and its location and size remain
unchanged. That layout check alone does not verify preservation or compatibility
of the installed settings across library and framework versions.

Before the first hardware upgrade:

1. Make a local full-flash backup for recovery. It includes network credentials
   and HomeKit pairing material; keep it private and outside Git.
2. Resolve the USB power arrangement described in [the hardware guide](hardware.md#checks-and-limits).
   Disconnect all three heat-pump leads and remove the ESP32 from the circuit
   for USB flashing, unless an alternative isolation arrangement has been checked.
3. Use the PlatformIO USB upload command in [building.md](building.md#flash-over-usb)
   so the bootloader, partition table, OTA boot metadata, and application are
   updated together. Do not use an application-only OTA upload to change layouts.
   Do not run a whole-flash erase as part of the upgrade if retaining settings.
4. Check startup, reset stability, saved Wi-Fi settings, and HomeKit pairing on
   USB power. A disconnected controller bus cannot validate heat-pump control.
5. Disconnect USB before restoring the heat-pump connections. Check secondary
   binding, thermostat updates, and synchronisation with the wall controller.

The backup, upload, and runtime checks above have **not** been performed for
this upgrade. The connected ESP32 was not opened, reset, or flashed during
this build investigation.

## Wi-Fi follow-up

The new framework's ESP32 SDK configuration enables WPA3-SAE. That establishes
build support, not successful interoperability with the owner's Aruba Instant
On access point. Test mixed WPA2/WPA3 authentication and reconnect behaviour
separately; do not infer a Wi-Fi fix from compilation.

## Upstream references

- [Espressif32 8.0.0 release](https://github.com/platformio/platform-espressif32/releases/tag/v8.0.0)
  identifies the Arduino 3.3.12 and toolchain upgrade.
- [HomeSpan 1.9.1 version guard](https://github.com/HomeSpan/HomeSpan/blob/release-1.9.1/src/version.h)
  requires Arduino-ESP32 2.x.
- [HomeSpan 2.1.8 version guard](https://github.com/HomeSpan/HomeSpan/blob/2.1.8/src/version.h)
  requires Arduino-ESP32 3.3.0 or later.
- [Arduino default partitions](https://github.com/espressif/arduino-esp32/blob/3.3.12/tools/partitions/default.csv)
  and [Minimal SPIFFS partitions](https://github.com/espressif/arduino-esp32/blob/3.3.12/tools/partitions/min_spiffs.csv).
- [HomeSpan partition FAQ](https://github.com/HomeSpan/HomeSpan/blob/2.1.8/docs/FAQ.md)
  explains the larger image and the need for USB when changing partition layouts.
