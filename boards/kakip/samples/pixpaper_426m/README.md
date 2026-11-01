# PIXPAPER-426-M e-paper (Zephyr display + LVGL) on the Kakip Cortex-M33

Drive an **Open-EP PIXPAPER-426-M** 4.26" monochrome e-paper panel (800×480)
from the Kakip's Cortex-M33 over **RSPI0**. Unlike [pixpaper_213m](../pixpaper_213m/),
the panel is a real Zephyr **display device** (`zephyr,display`), so anything
that uses the display API runs on it, including **LVGL**.

> Setup (SDK, `west update`), the console wiring and the U-Boot line that starts
> the M33 are in the [board README](../../README.md). Do those first. LVGL also
> needs `west update lvgl`.

**Status**: builds; not yet tried on hardware.

## Apps

| App | What it does |
| --- | ------------ |
| [image](apps/image/) | The image from the openep Linux demo with a full refresh, then a progress bar drawn with partial refreshes, timed. |
| [lvgl](apps/lvgl/) | LVGL: an e-paper dashboard that updates once a second, or LVGL's own widgets demo. |

`common/` holds the display driver (`pixpaper_426m.c`) and the wiring overlay.
`dts/bindings/` holds the panel binding; every app adds this directory to
`DTS_ROOT`, so nothing has to be copied into the Zephyr tree. The `open-ep`
vendor prefix does, see [pixpaper_213m](../pixpaper_213m/README.md#install-into-a-zephyr-tree).

## Wiring

Identical to [pixpaper_213m](../pixpaper_213m/README.md#wiring) and to Kakip's
Linux PIXPAPER-426-M overlay, so one set of wires serves all three:

| Panel | RZ/V2H pin | CN6 pin |
| ----- | ---------- | ------- |
| VCC   | 3.3 V      | 1 or 17 |
| GND   | GND        | 6 (or any GND) |
| CLK   | P9_2       | 23 |
| DIN   | P9_0       | 19 |
| CS    | P9_3       | 24 |
| DC    | P7_4       | 29 |
| RST   | P10_7      | 31 |
| BUSY  | P11_3      | 37 |

SPI runs at 20 MHz (`spi-max-frequency` in `common/kakip_cm33.overlay`), the
speed Kakip's Linux driver uses on this same wiring. The whole 48000-byte frame
goes out as one transfer. If the image breaks up, lower the clock (4 MHz is
proven with the 2.13" panel).

## How the driver refreshes

The sequence is the one proven on this panel by the Linux DRM driver (imx93 and
Kakip):

- **First update: full refresh.** The panel is reset and initialized, the frame
  goes to RAM `0x24` and to `0x26`, `0x21 = 0x40` makes the OTP waveform drive
  every pixel, then `0x22 = 0xF7`. The panel flashes for about 4 s.
- **Then, once: enter partial mode.** `0x21` back to `0x00`, RAM ping-pong on
  (`0x37`), a short custom LUT, analog powered up.
- **Every later update: partial.** The whole frame goes to `0x24`, then
  `0x22 = 0x0C`. The controller drives only the pixels that changed.

LVGL redraws only the areas that changed, in several pieces inside
`display_blanking_on/off`; the driver collects them and refreshes the panel
once, at `blanking_off`. Every 10 partial refreshes it logs the average time,
split into SPI and panel.

Knobs at the top of `common/pixpaper_426m.c`:

| Define | Default | Meaning |
| ------ | ------- | ------- |
| `EPD_FULL_EVERY` | 0 | Full refresh every N partials to clear ghosting; 0 = only the first. |
| `EPD_TP` / `EPD_FR` | 4 / 4 | Partial LUT drive length and frame rate, as in the Linux driver. |
| `EPD_CHUNK` | 48000 | Bytes per SPI transfer (the whole frame). |

## Build and run

```bash
cd ~/zephyrproject
HERE=path/to/this/repo/boards/kakip/samples/pixpaper_426m

.venv/bin/west build -p always -b rzv2h_evk/r9a09g057h44gbg/cm33 -d build-426-image $HERE/apps/image
.venv/bin/west build -p always -b rzv2h_evk/r9a09g057h44gbg/cm33 -d build-426-lvgl  $HERE/apps/lvgl

# LVGL's own widgets demo instead of the dashboard:
.venv/bin/west build -p always -b rzv2h_evk/r9a09g057h44gbg/cm33 -d build-426-widgets $HERE/apps/lvgl \
    -- -DCONFIG_LV_USE_DEMO_WIDGETS=y
```

Copy `zephyr/zephyr.bin` from the build directory to the SD card as
`kakip_cm33.bin` and start the M33 with the board README's Cortex-M33 line.

Expected log on SCI5 for `image`:

```text
<inf> app: Full refresh of the image...
<inf> pixpaper_426m: full refresh: ~4000 ms
<inf> app: Image shown in ~4000 ms
<inf> app: round 0: 25 partial refreshes, ... ms each on average
```

and for `lvgl`: one full refresh that puts up the first LVGL screen,
`LVGL running`, then one partial refresh a second.

## Memory used

| App | M33 SRAM (of ~1 MB) |
| --- | ------------------- |
| image | ~190 KB |
| lvgl (dashboard) | ~580 KB |
| lvgl (widgets demo) | ~600 KB |

LVGL takes most of it: 128 KB heap, ~90 KB of fonts, a 25%-of-screen draw
buffer plus its 1-bit conversion buffer, and the driver's 48 KB frame. The M33 has no flash, so the code is in SRAM too.

## If the panel stays blank

Same order as [pixpaper_213m](../pixpaper_213m/README.md#if-the-panel-stays-blank).
`BUSY stuck high` means the panel never answered the reset; no log at all
means the M33 is not running.
