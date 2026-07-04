# ESP OLED Clock

Five-screen digital clock built with a Seeed Studio XIAO ESP32-C6, a PCA9548A I2C multiplexer, and SSD1306 OLED displays.

The clock connects to Wi-Fi, syncs time from NTP, and displays one clock digit per OLED with a blinking colon in the middle. If Wi-Fi is not configured or cannot connect, the ESP32-C6 starts a setup hotspot and serves a small web page where Wi-Fi credentials can be entered.

## Features

- ESP-IDF firmware built with PlatformIO.
- Five OLED clock face: `HH:MM`.
- PCA9548A I2C multiplexer support.
- SSD1306 OLED drawing without an external display library.
- Seven-segment style custom digit rendering.
- Wi-Fi credentials saved in NVS flash.
- Setup hotspot when Wi-Fi is missing or unavailable.
- Setup details shown on the first OLED:
  - SSID: `CLOCK-SETUP`
  - Password: `CLOCKSETUP`
  - IP: `192.168.4.1`
- Web setup page for entering home Wi-Fi credentials.
- Automatic restart after credentials are saved.
- Clock continues running from the ESP32 system time if Wi-Fi disconnects after NTP sync.
- Small no-internet icon on the colon display during runtime Wi-Fi loss.
- UK/Ireland timezone configured with automatic GMT/BST switching.

## Hardware

- Seeed Studio XIAO ESP32-C6
- PCA9548A 8-channel I2C multiplexer
- 5x SSD1306 I2C OLED displays
  - 4x 128x64 OLEDs for clock digits
  - 1x 128x32 OLED for the colon/status display
- 3.3 V power supply with enough current for the ESP32-C6 and OLEDs
- I2C pull-ups, if not already present on the modules

See [docs/WIRING.md](docs/WIRING.md) for the exact wiring and display channel map.

## Setup Mode

Setup mode starts when:

- no saved Wi-Fi credentials exist,
- the ESP32-C6 cannot connect to the saved Wi-Fi network during boot,
- or initial NTP sync fails.

In setup mode:

1. Connect your phone or computer to Wi-Fi network `CLOCK-SETUP`.
2. Use password `CLOCKSETUP`.
3. Open `http://192.168.4.1`.
4. Enter your home Wi-Fi name and password.
5. Submit the form.
6. The ESP32-C6 saves the credentials and restarts.

The setup details are also displayed on the first OLED screen in small text.

## Build

Install PlatformIO, then run:

```sh
platformio run
```

The project target is configured in `platformio.ini`:

```ini
[env:seeed_xiao_esp32c6]
platform = espressif32
board = seeed_xiao_esp32c6
framework = espidf
```

## Flash

Connect the XIAO ESP32-C6 by USB and run:

```sh
platformio run --target upload
```

Open the serial monitor with:

```sh
platformio device monitor
```

The firmware prints I2C scan results, Wi-Fi status, setup hotspot details, and current time logs.

## Important Notes

- The setup hotspot password is stored in the source code as `CLOCKSETUP`.
- Home Wi-Fi credentials entered through the web page are saved in ESP32 NVS flash.
- The PCA9548A address is expected to be `0x70`, with A0/A1/A2 tied to GND.
- OLED address is expected to be `0x3C`.
- I2C uses GPIO16 for SDA and GPIO17 for SCL.
- The first OLED setup IP line is drawn with mirrored character placement to match the physical mounting/orientation.

## Source Layout

- `src/main.c` - firmware, Wi-Fi setup portal, NTP sync, OLED rendering, and clock loop.
- `platformio.ini` - PlatformIO board/framework configuration.
- `docs/WIRING.md` - hardware wiring and display mapping.

