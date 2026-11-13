# Elecrow CrowPanel 1.28" rotary: hardware facts for ESP-IDF work

The "CrowPanel 1.28inch-HMI ESP32 Rotary Display": a round touch screen set in a rotary knob,
with a push switch (the whole display presses in) and a ring of RGB LEDs behind it.

Sources, read on 2026-11-05: Elecrow's wiki page
[CrowPanel 1.28inch-HMI ESP32 Rotary Display](https://elecrow.com/wiki/CrowPanel_1.28inch-HMI_ESP32_Rotary_Display.html),
the Makerguides article
[Getting started with the CrowPanel 1.28" rotary display](https://www.makerguides.com/getting-started-crowpanel-1-28inch-hmi-esp32-rotary-display/)
(pin table and Arduino code), and RIOT-OS's board definition
[elecrow-crowpanel-esp32s3-rotary-128](https://api.riot-os.org/elecrow-crowpanel-esp32s3-rotary-128_2include_2board_8h.html).
The schematic was not read.

## The unit here

- ESP32-S3R8 (QFN56), rev v0.2: 8 MB octal PSRAM in the package (AP_3v3), 16 MB flash
  (ID 0xBA / 0x4018). MAC `68:ee:8f:5d:c1:d4`.
- USB is the chip's own USB Serial/JTAG (VID 0x303A, PID 0x1001): `/dev/cu.usbmodem*`.
  `idf.py flash` works as is; the console is on the same port.
- It came with an earlier Kobento build for this panel (log tag `knobpanel`); erased.

## Pins

| What | GPIO | Notes |
|---|---|---|
| LCD SCLK / MOSI / CS / DC / RST | 10 / 11 / 9 / 3 / 14 | GC9A01, SPI2, 80 MHz |
| LCD backlight | 46 | LEDC PWM, active high |
| LCD 3V3 enable | 1 | active high; must be on before the panel init |
| LED rail enable | 2 | active high (Makerguides: "LEDA_3V3"; RIOT: external headers' 5 V) |
| Touch SDA / SCL / INT / RST | 6 / 7 / 5 / 13 | CST816D at 0x15, same registers as CST816S |
| Encoder A / B | 45 / 42 | PCNT x4 decoding, internal pull-ups |
| Encoder switch | 41 | active low, internal pull-up |
| RGB LEDs | 48 | 5 x WS2812 (GRB), RMT at 10 MHz |
| Red power LED | 40 | active low |
| BOOT | 0 | |

## What the player does with it (board/knob.h)

- `knobPosition()`: detents (4 counts each), + clockwise. `knobShortPresses()` /
  `knobLongPresses()`: the switch, debounced and timed by a 5 ms timer, so a press is caught
  even while a 3D document takes most of a second per frame.
- `setLeds()`: the ring, clockwise from the top.
- rcplayer turns them into the `Control.knob*`, `Control.press`, `Control.led*` named
  variables (docs/RC_SYSTEM_VARIABLES.md) and boots into `documents/knob/knob_menu`.
- Console extras: `knob` (prints counts, position and switch for 15 s), `turn N`, `press`,
  `hold` (simulate the knob), `led RRGGBB` / `led off`, `touch`, `i2c`.

## The UART header and the link to another board

The 4-pin UART header's TX is **GPIO43** (UART0 TX; found by a link search with the header
wired to another board): free for a one-wire link to another board (open-drain). The chip's
ROM may print its boot log on GPIO43 at reset; a 470 Ω-1 kΩ series resistor on the wire
protects both ends.

## Rotation

`kRotation` in `components/board/src/board.cpp` turns the picture clockwise by 0, 90, 180 or 270
degrees, set to 270 by default (it was 90 until 2026-11-12, when the knob was turned round). The panel does the turning through MADCTL, so it costs nothing per
frame, and `readTouch` turns touch coordinates to match. Checked on the device at 90; at 270 the picture is checked, touch not yet. Screenshots
are taken before the panel, so they stay upright.

## Not verified yet

- Counts per detent (4 assumed). The direction is checked: clockwise counts up as wired.
- The LED order round the ring (clockwise from the top assumed) and their colour order (GRB).
