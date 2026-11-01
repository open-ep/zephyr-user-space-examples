# pixpaper-213m e-paper (raw SPI) on the Kakip Cortex-M33

Drive an **Open-EP pixpaper-213m** 2.13" monochrome e-paper panel (250×122) from
the Kakip's Cortex-M33, over the RZ/V2H's **RSPI0** controller, with DC/RST/BUSY
as plain GPIOs. The panel command sequence is issued by hand, no display
subsystem.

> Setup (SDK, `west update`), the console wiring and the U-Boot line that starts
> the M33 are in the [board README](../../README.md). Do those first.

**Verified on hardware** (2026-10-01): the image shows on a pixpaper-213m wired as below.

## Apps

| App | What it does |
| --- | ------------ |
| [image](apps/image/) | Show a bundled image (`png_HEX.h`). |
| [refresh-bench](apps/refresh-bench/) | Partial-refresh speed test: a moving bar, with SPI time, panel time and fps per waveform length. |

All apps share the panel binding (`binding/`), the vendor-prefix patch
(`patches/`), and the wiring overlay (`common/`).

## Wiring

The pins are the ones Kakip's own Linux `kakip-es1-pixpaper-overlay.dts` uses,
so one set of wires works for both Linux and this firmware. All on the 40-pin
header **CN6**; the GPIOn names are the ones printed on Kakip's pin map.

| Panel | RZ/V2H pin | CN6 pin | Kakip name | Notes |
| ----- | ---------- | ------- | ---------- | ----- |
| VCC   | 3.3 V      | 1 or 17 | 3V3    | panel is 3.3 V only |
| GND   | GND        | 6, 9, 14, 20, 25, 30, 34 or 39 | GND | |
| CLK   | P9_2       | 23      | GPIO11 | RSPI0 clock |
| DIN   | P9_0       | 19      | GPIO10 | RSPI0 MOSI |
| CS    | P9_3       | 24      | GPIO8  | driven as a GPIO, active low |
| DC    | P7_4       | 29      | GPIO5  | low = command, high = data |
| RST   | P10_7      | 31      | GPIO6  | low = reset |
| BUSY  | P11_3      | 37      | GPIO26 | high = busy |

SPI runs at 4 MHz, mode 0, defined by `common/kakip_cm33.overlay` (shared by
every app under `apps/`). The overlay also keeps the M33 console on SCI5 (CN6
pin 8/10), so the log and the panel can be used at the same time.

## Install into a Zephyr tree

The panel binding and the `open-ep` vendor prefix have to be in the Zephyr tree
(the apps themselves can stay in this repo):

```bash
ZEPHYR=~/zephyrproject/zephyr
HERE=/path/to/this-repo/boards/kakip/samples/pixpaper_213m

# 1. Panel devicetree binding
cp "$HERE"/binding/open-ep,pixpaper-213m.yaml "$ZEPHYR/dts/bindings/display/"

# 2. Register the vendor prefix (skip if `grep open-ep $ZEPHYR/dts/bindings/vendor-prefixes.txt` already matches)
cd "$ZEPHYR"
git apply "$HERE"/patches/0001-dts-bindings-add-open-ep-vendor-prefix.patch
```

Then build an app, see its README, e.g. [apps/image](apps/image/).

## If the panel stays blank

Work from the M33 log on SCI5 outwards:

1. **No log at all.** The M33 is not running. Go back to the board README
   (dual-core hello is the simplest check).
2. **Log stops at `Initializing pixpaper-213m over RSPI0...`** and after ~10 s
   prints `BUSY stuck high`. The panel never answered the reset: check 3V3,
   GND, RST (pin 31) and BUSY (pin 37).
3. **Log reaches `Displayed image`, panel still blank.** Data is not arriving:
   check CLK (23), DIN (19), CS (24), DC (29). Try a lower SPI clock
   (`spi-max-frequency` in the overlay) if the wires are long.
4. **Garbage or a band of wrong pixels.** Lower `EPD_CHUNK` in `src/main.c`
   (the number of bytes per SPI transfer).

Do not load Kakip's Linux pixpaper overlay while this firmware runs: Linux and
the M33 would both drive RSPI0 and the same three GPIOs. With the A55 at the
U-Boot prompt, as in the board README, nothing else touches them.
