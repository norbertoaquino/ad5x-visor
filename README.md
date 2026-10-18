# AD5X Visor

A round-screen status monitor for the Flashforge AD5X. It runs on a Waveshare ESP32-S3-LCD-1.28 (240×240 GC9A01, no touch) and shows the live print from the printer on your LAN.

The screen rotates through the pages that apply to the current job:

- progress, status, layer, and file name
- nozzle and bed temperature
- elapsed and remaining time, only while the status is `printing`
- the slicer thumbnail, after it has downloaded
- an offline page when Wi-Fi is up and the printer does not answer ping

Wi-Fi down is a separate page. It is not the printer-offline page.

## Hardware

| Signal | GPIO |
| --- | --- |
| DC | 8 |
| CS | 9 |
| SCLK | 10 |
| MOSI | 11 |
| RST | 12 |
| Backlight | 40 |

The panel is write-only. The BOOT button toggles the serial log while the firmware is running.

## Configure

Copy the example and fill in your network and printer:

```sh
cp src/config.example.h src/config.h
```

`src/config.h` is gitignored. Do not commit it.

| Setting | Meaning |
| --- | --- |
| `WIFI_SSID`, `WIFI_PASS` | The 2.4 GHz network |
| `WIFI_CHANNEL` | Optional. Delete the line and the radio picks the channel |
| `WIFI_DHCP` | `1` for DHCP, `0` for the static address below |
| `WIFI_IP`, `WIFI_GW`, `WIFI_MASK`, `WIFI_DNS` | Used when `WIFI_DHCP` is `0` |
| `PRINTER_IP` | The AD5X address |
| `PRINTER_SERIAL`, `PRINTER_CHECK` | Serial number and check code from the Flashforge app |
| `APP_LANG` | Screen and log language |
| `DEBUG_LOG` | `true` prints the serial log from boot. Default is `false` |

Languages:

| Code | Language |
| --- | --- |
| `en` | English |
| `pt` | Portuguese |
| `es` | Spanish |
| `ru` | Russian |
| `zh` | Chinese |
| `ja` | Japanese |
| `tr` | Turkish |

An unknown code falls back to English.

## Build and flash

[PlatformIO](https://platformio.org/) with the Espressif 32 platform:

```sh
pio run -t upload
pio device monitor -b 115200
```

The board is a 16 MB ESP32-S3. The serial log is 115200. With `DEBUG_LOG` set to `false`, press BOOT to turn the log on or off.

## Add a language

1. Copy `src/lang/en.h` to `src/lang/xx.h`.
2. Translate every string. Keep each `%` placeholder in the same order.
3. Include the new file in `src/i18n.cpp` and add `{"xx", kXx}` to `kLangs`.
4. Set `APP_LANG` to `xx`.

`MSG_UI_*` strings are drawn on the screen. The other strings are the serial log. An empty translation falls back to English.

The round panel draws screen text from `src/ui_font_data.h`. That font already contains the characters used by the languages above. A translation that introduces a new character will not show that character until the font file is rebuilt.

## What it talks to

The firmware pings `PRINTER_IP`. When ping answers, it posts to `http://<printer>:8898/detail` about every 30 seconds and reads progress, temperatures, layer, file name, elapsed time, and remaining time. The thumbnail is downloaded once per file. A missed detail response keeps the last good reading. The offline page appears only when ping itself gets no reply.

## Logo

The orange mark on the offline page is the Flashforge logo, drawn from the [Simple Icons](https://simpleicons.org/) path. Simple Icons artwork is [CC0](https://creativecommons.org/publicdomain/zero/1.0/).
