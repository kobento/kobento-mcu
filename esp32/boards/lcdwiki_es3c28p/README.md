# LCDWIKI ES3C28P — 2.8inch ESP32-S3 Display

ESP32-S3 N16R8 (8 MB octal PSRAM, 16 MB flash), 2.8" 240x320 ILI9341V on SPI (driven as
320x240 landscape), FT6336G capacitive touch, ES8311 codec + FM8002E amplifier, microSD
(SDIO), one WS2812, BOOT and RESET keys. One USB-C, wired to the chip's USB Serial/JTAG.

    idf.py -B build/lcdwiki_es3c28p -DBOARD=lcdwiki_es3c28p -p /dev/cu.usbmodem* flash

Pins from the LCDWIKI page "2.8inch ESP32-S3 Display" (not checked against the schematic):

| Function | GPIO |
|---|---|
| LCD SCLK / MOSI / MISO / CS / DC | 12 / 11 / 13 / 10 / 46 |
| LCD RST | the chip's EN (software reset 0x01 instead) |
| Backlight (high = on) | 45 |
| Touch SDA / SCL / RST / INT (FT6336G at 0x38) | 16 / 15 / 18 / 17 |
| ES8311 (I2C 0x18 on the same bus); I2S MCLK/BCLK/WS/DOUT/DIN | 4 / 5 / 7 / 8 / 6 |
| Amplifier enable (low = on) | 1 |
| WS2812 | 42 |
| BOOT key (steps to the next document) | 0 |
| SD CLK / CMD / D0–D3 | 38 / 40 / 39, 41, 48, 47 |
| Battery ADC | 9 |
| Link to a second board (one wire) | 44 (UART0 RX, serial header) |

## Teensy Audio Shield (optional)

`src/audio.cpp` drives an external Teensy Audio Shield (SGTL5000) wired to the expansion
headers. The board breaks out only four spare GPIOs, so MCLK borrows UART0 TX (the console
is on USB, so nothing is lost):

| Shield pad | Rev D/D2 (Teensy 4) | Rev C (Teensy 3) | ESP32-S3 |
|---|---|---|---|
| MCLK (12.288 MHz) | 23 | 11 | IO43 (TXD, serial header) |
| BCLK | 21 | 9 | IO14 (P3) |
| LRCLK | 20 | 23 | IO21 (P3) |
| RX (ADC data to the ESP32) | 8 | 13 | IO2 (P3) |
| TX (DAC data from the ESP32) | 7 | 22 | IO3 (P3) |
| SDA / SCL | 18 / 19 | 18 / 19 | IO16 / IO15 (P4) |
| 3.3V, GND | | | 3V3, GND (P4); never 5 V |

I2S0 runs as master at 48 kHz, 16-bit data in 32-bit slots (BCLK 64 Fs, MCLK 256 Fs). The
SGTL5000 answers I2C (0x0A) only while MCLK runs, so I2S starts before the probe; with no
shield the probe fails and `beep()` returns false. The codec setup is the Teensy Audio
library's `enable()` sequence at SYS_FS 48 kHz: headphones out (volume 50 %), the mic in
(3.0 V bias, +40 dB), ADC to I2S and I2S to DAC. Keep MCLK and BCLK under ~10 cm.

Console: `audio` (chip ID and power), `tone [hz] [ms]`, `volume [hp|spk] 0..100`,
`out hp|spk|both` (where tones and playback go), `mic` / `line` (select the input, then print
its level for 5 s), `rec [s]` (record and measure), `play`, `wav`, `loop [hz]` (the speaker
plays a tone and the mic listens for it), `spk`. `tools/rec.py` records you speaking, saves
a WAV and plays it back on the board.

## The board's speaker

`src/speaker.cpp` drives the on-board ES8311 (0x18) on I2S1 (MCLK IO4, BCLK IO5, LRCK IO7,
data IO8) and enables the FM8002E amplifier (IO1 low). Register values are those of
Espressif's esp_codec_dev es8311 driver (slave, 12.288 MHz MCLK, 48 kHz, 16-bit).

Checked with `loop` (tones on the speaker only, heard by the shield's mic): 500 Hz +30 dB,
1 kHz +42 dB, 2 kHz +34 dB, 4 kHz +39 dB above silence. Before that, a mic wired to a jack
board's switch pin (LN/LS) instead of the tip left MIC floating: 60 Hz hum at -25 dBFS with
+40 dB gain, and nothing heard. Line in reads -85 dBFS with nothing connected.

Checked here: the codec answers (ID 0xA011), the mic hears the board's speaker (below), the
mic's noise floor is about -40 dBFS at +40 dB (mostly 60 Hz hum from the jumper wires).
The shield's headphones were checked by ear on 2026-11-08 (a mic monitor app):

- Both ears play only with `ws_width = 32`. The ESP-IDF Philips preset sets the LRCLK pulse
  to the data width (16 of 64 BCLKs), and the codec then read the right slot late.
- A steady tick, about 17 Hz, is electrical rather than digital. The test console's `loopz`
  (codec I2S in looped to I2S out) returned both slots sample-exact. The tick stayed with
  digital silence playing, grew with the headphone volume, and went away with the DAC muted
  (CHIP_ADCDAC_CTRL 0x000C). It didn't change with the display, the CPU load, the link, the
  speaker amp or I2S1. The likely path is supply noise on the 3.3 V jumper to the shield; not
  yet confirmed with a capacitor at the shield (100 µF + 100 nF suggested).

## The unit here

- MAC `7c:e8:b1:b2:61:64`; came with LCDWIKI's Arduino factory test (`2.8_ESP32S3_AP`).
- I2C scan: 0x18 (ES8311), 0x38 (FT6336G).

## Not verified

- Landscape direction (MADCTL 0xA8, `kRotate180` in board.cpp flips it) and the touch
  mapping that follows it (`touch` on the console prints raw and screen coordinates).
- Colours (INVON + BGR, as LCDWIKI's init).
- Not driven yet: the on-board ES8311 and speaker, SD, the WS2812, battery.
