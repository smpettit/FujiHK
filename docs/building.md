# Building FujiHK on Linux

For the XC3800 circuit, parts, and wiring, see [the hardware guide](hardware.md).

## Supported build environments

| Environment | Board | Setup button | Status pixel |
| --- | --- | --- | --- |
| `esp32dev` (default) | Jaycar Duinotech XC3800, ESP32-WROOM-32E | BOOT / GPIO0 | Disabled; no pixel fitted |
| `m5stack-atom` | Original M5Atom | GPIO39 | GPIO27 |

Both builds use GPIO19 for LIN RX and GPIO22 for LIN TX. They retain the
upstream secondary-controller mode, for use alongside an existing wired wall
controller. The XC3800's EN button resets the board; BOOT is its setup button.

The current build uses PlatformIO Espressif32 **8.0.0** (Arduino-ESP32
**3.3.12**, ESP-IDF **5.5.5**) and HomeSpan **2.1.8**, with the framework's
`min_spiffs.csv` partition layout. Both boards compile with these versions.
The XC3800 has also passed USB startup, saved-settings, and mixed-mode Wi-Fi
checks with the heat-pump leads disconnected. Heat-pump control and M5Atom
runtime checks remain pending. See [the upgrade findings](toolchain-upgrade.md)
for the reproduced failures, partition changes, and hardware results.

The owner's working hardware baseline used Espressif32 **6.13.0**, Arduino-ESP32
**2.0.17**, and HomeSpan **release-1.9.1**. The current platform and HomeSpan
versions are pinned in `platformio.ini`. FujiHeatPump still follows the upstream
Git repository's default branch, so its revision can change between fresh installs.
Sibling FujiHeatPump, HomeSpan, and platform-espressif32 checkouts are not used
by these builds; PlatformIO downloads the dependencies in `platformio.ini`.

## Install PlatformIO Core

Requirements: Git, Python 3 with `venv` and `pip` support, an internet connection,
and a USB data cable for flashing. Installation needs no system-wide Python
packages. From the root of this repository:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install platformio==6.1.19
```

If Python reports that `venv` is unavailable, install your Linux distribution's
Python virtual-environment package first. PlatformIO downloads the cross compiler,
Arduino framework, libraries, and upload tools on the first build.

## Build

For the XC3800:

```sh
.venv/bin/pio run -e esp32dev
```

For the original M5Atom:

```sh
.venv/bin/pio run -e m5stack-atom
```

Build outputs are under `.pio/build/<environment>/`. Use PlatformIO's upload
command below so that the bootloader, partition table, and application are
written at the correct offsets; `firmware.bin` alone is not a complete flash image.

## Flash over USB

When upgrading from the older build, review [the partition migration notes](toolchain-upgrade.md#partition-layout-and-first-upgrade)
first. This upgrade changes the partition table and requires USB flashing.

For this externally powered XC3800 build, **never connect USB while the heat-pump
12 V supply is connected**. Remove the ESP32 from the breadboard before connecting
USB. After flashing, disconnect USB before reinstalling it in the circuit.

With the board connected by USB, list the available ports:

```sh
.venv/bin/pio device list
```

Use the port shown by that command (replace `/dev/ttyUSB0` if necessary):

```sh
.venv/bin/pio run -e esp32dev -t upload --upload-port /dev/ttyUSB0
.venv/bin/pio device monitor -p /dev/ttyUSB0 -b 115200 --echo
```

The XC3800 uses a CP2102 USB-to-serial interface. If the port is missing or access
is denied, check USB cable, driver, and serial-device permissions using your
distribution's instructions. Do not run PlatformIO as root. If automatic upload
reset fails, hold BOOT, press and release EN, then release BOOT once the uploader
connects. Exit the serial monitor before uploading again.

## Configure HomeKit and Wi-Fi

In the serial monitor, use HomeSpan's commands:

- `S <pin>` sets your eight-digit HomeKit pairing code.
- `W` starts interactive Wi-Fi configuration.

Follow the serial prompts and keep credentials out of Git and shared logs. The
XC3800 has no status pixel, so use serial output for setup feedback. GPIO0 is
also a boot-strapping pin: holding BOOT during reset enters the ESP32 downloader.

The older firmware failed to connect when WPA2/WPA3 mixed mode was enabled on
the owner's Aruba Instant On network. With the upgraded stack, the XC3800
connected after a hardware reset and after a forced Wi-Fi disconnect with
mixed mode enabled. Existing credentials and HomeKit pairing were retained.
See [the hardware validation results](toolchain-upgrade.md#hardware-validation)
for the observations and limits. WPA3-only operation and the protocol actually
negotiated on the mixed network have not been verified.

## Validation limits

Both environments compiled successfully on Linux with PlatformIO Core 6.1.19
and Python 3.14. The resolved FujiHeatPump revision was `72e00ec` and HomeSpan
revision was `107ffc0`.

| Environment | RAM used | Application flash used (limit 1,966,080 bytes) |
| --- | --- | --- |
| `esp32dev` | 62,036 bytes | 1,642,008 bytes (83.5%) |
| `m5stack-atom` | 62,724 bytes | 1,658,801 bytes (84.4%) |

The larger application slots are necessary for the upgraded firmware; the
older default partition layout does not fit it.

A successful build verifies compilation and image size. It does not verify
physical wiring, Wi-Fi authentication, HomeKit operation, or communication with
the heat pump. Recheck those on hardware after flashing. This change preserves
the existing thermostat and fan source code; heat-pump behaviour still needs checking.
