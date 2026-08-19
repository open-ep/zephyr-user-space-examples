# pixpaper-213c e-paper (4-colour, bit-banged SPI)

Drive an **Open-EP pixpaper-213c** 2.13" four-colour e-paper panel (250x122,
black / white / yellow / red) on the W6300-EVB-Pico2 / Raspberry Pi Pico 2.

> Platform prerequisites — toolchain, Zephyr workspace, wiring and `.uf2`
> flashing — are in the [board README](../../README.md).

Different controller family from the mono panel, with two consequences worth
knowing before you debug anything:

- **BUSY polarity is inverted**: this controller idles **HIGH**, busy = LOW.
- **A full refresh takes 15-25 s.** That is normal, not a hang. There is no
  partial-update mode here, so every update is a full refresh.

Pixels are 2 bits (`00` black, `01` white, `10` yellow, `11` red), streamed
column-major with the horizontal mirror already baked into the packed data —
7750 B per frame, which is 0.4 % of this board's flash. (The same image does not
fit alongside Zephyr on a 16 KB CH32V003, which is why the 4-colour sample only
exists on this board.)

## Apps

| App | What it does |
| --- | ------------ |
| [quick-update](apps/quick-update/) | Alternate the bundled dithered image and a black/white/red/yellow bar pattern every 30 s. |

## Colour and dithering

The panel has only four ink states and cannot mix colours per pixel, so
intermediate colours come from spatial dithering: red+yellow reads as orange,
red+white as pink, black+white as gray, red+black as brown. Blue does not exist
in the ink at all — blue content is best rendered as equal-luminance gray.

The host-side converter with Floyd-Steinberg dithering, plus recipes for flat
artwork versus photographs, lives in the Arduino examples repo
([uiap/pixpaper-213-c/mix-color](https://github.com/open-ep/arduino-user-space-examples/tree/main/uiap/pixpaper-213-c/mix-color)).
The packed `img_packed.h` it produces is interchangeable with this app — the
firmware cannot tell a dithered image from a flat one.
