# pixpaper-213m e-paper (bit-banged SPI)

Drive an **Open-EP pixpaper-213m** 2.13" monochrome e-paper panel (250×122) on
the UIAPduino Pro Micro CH32V003 — no display subsystem and no SPI driver: the
panel is a slow write-only device and the CH32V003 has only 16KB flash / 2KB
SRAM, so SPI mode 0 is bit-banged over plain GPIOs and the image is streamed
from flash with no framebuffer.

> Platform prerequisites — the board-support setup (PR branch), the **3.3V Volt-Sel
> rework**, and the USB-bootloader flashing flow — are in the
> [board README](../../README.md). Do those first.

## Apps

| App | What it does |
| --- | ------------ |
| [console](apps/console/) | Draw a bundled image at boot, then serve an interactive UART console (redraw, fill, load a new image over UART, diagnostics). |

All apps share the wiring overlay (`common/`). The host-side PNG-to-frame
converter for the console's `load` command is in `tools/`.

## Wiring

| Signal | Pin (silkscreen) | CH32V003 GPIO | Notes                       |
| ------ | ---------------- | ------------- | --------------------------- |
| DIN    | 8                | PC6           | bit-banged MOSI             |
| CLK    | 7                | PC5           | bit-banged SCK, mode 0      |
| CS     | 6                | PC4           |                             |
| DC     | 5                | PC3           | low = command, high = data  |
| RST    | 10               | PD0           | low = reset                 |
| BUSY   | 12 (A3)          | PD2           | high = busy                 |
| VCC    | 3V3              | -             | panel is 3.3V-only          |
| GND    | GND              | -             |                             |

The console UART is USART1: TX = PD5 (silkscreen `TX`), RX = PD6 (`RX`),
115200 8N1, via an external 3.3V USB-UART dongle (the board's USB-C is
bootloader-only). All of this is defined by
`common/uiapduino_pro_micro_ch32v003.overlay` (shared by every app under
`apps/`).

## Install into a Zephyr tree

The apps are freestanding Zephyr applications — once the board support from
the [board README](../../README.md) is in place, you can build them **directly
from this repo** (see the app README), or copy them into the tree:

```bash
ZEPHYR=/path/to/zephyrproject/zephyr
HERE=/path/to/this-repo/boards/uiapduino/samples/pixpaper_213m

DST="$ZEPHYR/samples/boards/uiap/uiapduino/pixpaper_213m"
mkdir -p "$DST"
cp -r "$HERE"/apps "$HERE"/common "$HERE"/tools "$DST/"
```

No devicetree binding or vendor-prefix patch is needed: the panel pins are
plain GPIOs declared in the app-private `zephyr,user` node of the shared
overlay.

Then build an app — see its README, e.g. [apps/console](apps/console/).
