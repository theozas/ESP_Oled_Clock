# Wiring Guide

This document describes the hardware connections used by the ESP OLED Clock firmware.

## Main Parts

- Controller: Seeed Studio XIAO ESP32-C6
- I2C multiplexer: PCA9548A
- Displays: SSD1306 I2C OLED modules
- Display count: 5

## ESP32-C6 To PCA9548A

| ESP32-C6 / XIAO Pin | PCA9548A Pin | Purpose |
| --- | --- | --- |
| 3V3 | VIN / VCC | 3.3 V power |
| GND | GND | Ground |
| GPIO16 | SDA | I2C data |
| GPIO17 | SCL | I2C clock |

Firmware settings:

```c
#define I2C_SDA_GPIO 16
#define I2C_SCL_GPIO 17
#define I2C_FREQ_HZ 400000
```

## PCA9548A Address

The firmware expects the PCA9548A at address `0x70`.

This is the usual address when:

| PCA9548A Address Pin | Connection |
| --- | --- |
| A0 | GND |
| A1 | GND |
| A2 | GND |

Firmware setting:

```c
#define PCA9548A_ADDR 0x70
```

## OLED Address

Each OLED is expected to use I2C address `0x3C`.

Firmware setting:

```c
#define OLED_ADDR 0x3C
```

Because the OLEDs are behind the PCA9548A, multiple displays can share the same `0x3C` address as long as each one is connected to a different mux channel.

## OLED Channel Map

| Firmware Display | Clock Role | PCA9548A Channel | OLED Size | Rotation |
| --- | --- | ---: | --- | --- |
| `displays[0]` | Hour ones / setup info screen | 1 | 128x64 | 270 degrees |
| `displays[1]` | Hour tens | 0 | 128x64 | 270 degrees |
| `displays[2]` | Colon / no-internet icon | 2 | 128x32 | 90 degrees |
| `displays[3]` | Minute tens | 3 | 128x64 | 90 degrees |
| `displays[4]` | Minute ones | 4 | 128x64 | 90 degrees |

Code reference:

```c
static oled_t displays[5] = {
    { .channel = 1, .width = 128, .height = 64, .rotation = ROT_270 },
    { .channel = 0, .width = 128, .height = 64, .rotation = ROT_270 },
    { .channel = 2, .width = 128, .height = 32, .rotation = ROT_90  },
    { .channel = 3, .width = 128, .height = 64, .rotation = ROT_90  },
    { .channel = 4, .width = 128, .height = 64, .rotation = ROT_90  },
};
```

## PCA9548A To OLEDs

Connect each OLED to its assigned PCA9548A channel:

| PCA9548A Channel | Connect To |
| ---: | --- |
| 0 | Hour tens OLED |
| 1 | Hour ones OLED / setup info OLED |
| 2 | Colon OLED |
| 3 | Minute tens OLED |
| 4 | Minute ones OLED |

For every OLED:

| PCA9548A Channel Pins | OLED Pins |
| --- | --- |
| SDx | SDA |
| SCx | SCL |
| 3.3 V | VCC |
| GND | GND |

Use the matching `SDx`/`SCx` pair for the channel. For example, channel 1 uses `SD1` and `SC1`.

## Setup Hotspot Display

When the ESP32-C6 enters setup mode, only the first OLED (`displays[0]`, PCA9548A channel 1) shows setup information.

It displays:

```text
SSID
CLOCK-SETUP

PASS
CLOCKSETUP

IP
192.168.4.1
```

The other four OLEDs are cleared during setup mode.

## Runtime Wi-Fi Loss Indicator

After the clock has synced time once, it can continue displaying time if Wi-Fi disconnects. During Wi-Fi loss, the colon OLED (`displays[2]`, PCA9548A channel 2) shows a small no-internet mark while the time continues to update from the ESP32 system clock.

