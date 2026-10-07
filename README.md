# Firmware for the RP2040 + ESP8285 "Pico W" clone

Some cheap "Raspberry Pi Pico W" boards on AliExpress (e.g. sold by TZT) are not
Pico Ws: instead of the Infineon CYW43439 they carry an **ESP8285** WiFi chip
wired to the RP2040's UART0. Official Pico W firmware, MicroPython's `network`
module and the pico-sdk `cyw43` driver all fail on them (`[CYW43] Failed to start CYW43`).

The board actually has two microcontrollers. This repo programs both, with each one doing what it's good at:

| Chip | Code | Job |
|---|---|---|
| **RP2040** | `pico/`: C, pico-sdk 2.2 | 8x8 WS2812 matrix: text rendering, scroll timing, PIO output |
| **ESP8285** | `esp/`: PlatformIO, ESP8266 Arduino core | WiFi, web UI, telemetry, OTA updates |

There is no AT firmware here. The ESP runs its own program, and the two chips exchange text lines over UART.
**The application:** an 8x8 WS2812 matrix on GP22 that you control from your browser.
- **Scrolling text:** set the text, color, speed and brightness. "Surprise me" spreads a rainbow across the string.
- **Images:** pick any image. The browser crops it to a square, shrinks it to 8x8 and gamma-corrects it, then the matrix shows it.

On boot the matrix scrolls its IP address. A red dot means it isn't on WiFi yet.

The Pico handles all display timing, so scrolling stays smooth whatever WiFi is doing.

## Hardware

| Signal | RP2040 | ESP8285 |
|---|---|---|
| UART TX → RX | GP0 | RX |
| UART RX ← TX | GP1 | TX |
| WS2812 data (array **DIN**) | GP22 | — |

Matrix wiring is configured at the top of `pico/main.c`: `SERPENTINE`, `FLIP_X`, `FLIP_Y`.
The defaults assume LED 0 at the bottom right and every row running the same way. If text comes out mirrored or upside down, flip the matching knob.

The array is powered from VSYS, so `pico/power.h` caps the estimated LED current at
300 mA by scaling whole frames. Change `LED_BUDGET_MA` / `LED_MA_PER_CHANNEL` to match your supply.

## UART protocol (115200 8N1, `\n`-terminated)

| Direction | Line | Meaning |
|---|---|---|
| ESP → Pico | `WIFI <0\|1> <ip>` | every 1 s; on first connect the Pico scrolls the IP |
| ESP → Pico | `TEXT <rrggbb> <speed> <rainbow 0\|1> <text>` | scroll text, speed in columns/s (1–60); rainbow spreads hues over the string |
| ESP → Pico | `IMG <384 hex>` | 8x8 image, `rrggbb` per pixel, row-major from top-left |
| Pico → ESP | `T <text>` | telemetry, served at `/telemetry` |

Unknown lines are ignored, which covers the ESP's boot-ROM noise. On the Pico, UART RX is interrupt-driven into a ring buffer, so the main loop never blocks on the ESP.

## Web (http://pico-esp.local)

- `/`: the control page
- `/text?s=Hello&c=00ff40&v=12&r=0`: scroll text (`c` = color, `v` = columns/s, `r=1` = rainbow)
- `/img?d=<384 hex>`: show an 8x8 image
- `/telemetry`: latest Pico telemetry line

## Build & flash

Needs pico-sdk 2.2.0 (e.g. via the Raspberry Pi Pico VS Code extension, in `~/.pico-sdk`) and PlatformIO.

**Pico:**

    cd pico && cmake -S . -B build -G Ninja && ninja -C build
    picotool load -f -x build/pico_wifi_display.uf2   # first time: BOOTSEL + copy the .uf2

**ESP**, after the first serial flash, over WiFi:

    cp esp/src/secrets.h.example esp/src/secrets.h    # your SSID / password
    cd esp && pio run -e ota -t upload

### First ESP flash (serial, one time)

The ESP8285's UART only reaches USB through the RP2040, so the Pico has to act as a USB-serial bridge:

1. Flash `Serial_port_transmission.uf2` from [JiriBilek/RP2040_PicoW_ESP8285_Library](https://github.com/JiriBilek/RP2040_PicoW_ESP8285_Library/tree/main/firmware) to the Pico.
2. Unplug, hold the button **near the WiFi chip** (ESP GPIO0), plug back in.
3. `cd esp && pio run -t upload` (set `upload_port` in `platformio.ini`; uses DOUT flash mode)
4. Hold BOOTSEL, replug, copy `pico/build/pico_wifi_display.uf2` to the RPI-RP2 drive.

To go back to AT firmware, flash
[ESP_ATMod](https://github.com/JiriBilek/ESP_ATMod) the same way and use
JiriBilek's MicroPython library.

## Next steps

- Move the realtime work to core 1 (`multicore_launch_core1`).
- Move the LED updates to DMA. They currently block for about 30 µs per LED.
- Support non-ASCII text (currently shown as `?`); `font8x8_ext_latin.h` covers Latin-1.
- Push telemetry from the ESP to an internet service (MQTT etc.).

## Credits

- [mocacinno/rp2040_with_esp8285](https://github.com/mocacinno/rp2040_with_esp8285) and
  [JiriBilek/RP2040_PicoW_ESP8285_Library](https://github.com/JiriBilek/RP2040_PicoW_ESP8285_Library)
  worked out how to use these boards.
- `pico/ws2812.pio` © Raspberry Pi Ltd, BSD-3-Clause.
- `pico/font8x8_basic.h` from [dhepper/font8x8](https://github.com/dhepper/font8x8), public domain.

## License

MIT
