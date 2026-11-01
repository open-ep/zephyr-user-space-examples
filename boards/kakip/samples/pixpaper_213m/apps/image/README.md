# pixpaper-213m: image (raw SPI) on the Kakip Cortex-M33

Show a bundled image on the Open-EP pixpaper-213m e-paper panel.

This app maps `src/png_HEX.h` into the panel framebuffer and refreshes it: on
boot it initializes the panel, clears to white (to remove ghosting), renders the
image, and idles. E-paper keeps the image with no power.

Panel wiring, the devicetree binding, and how to install them into a Zephyr tree
are in the [panel README](../../README.md). Setup, the console wiring and the
U-Boot start line are in the [board README](../../../../README.md).

## Building

```bash
cd ~/zephyrproject
.venv/bin/west build -p always -b rzv2h_evk/r9a09g057h44gbg/cm33 -d build-epd \
    path/to/this/repo/boards/kakip/samples/pixpaper_213m/apps/image
```

The shared overlay (`../../common/kakip_cm33.overlay`, panel wiring + SCI5
console) is pulled in automatically by this app's `CMakeLists.txt`.

Copy `build-epd/zephyr/zephyr.bin` to the SD card's first partition as
`kakip_cm33.bin` and start the M33 with the Cortex-M33 line from the board
README. The log on SCI5 (CN6 pin 8/10, 115200 8N1):

```text
*** Booting Zephyr OS ... ***
[00:00:00.000,000] <inf> pixpaper_213m: Initializing pixpaper-213m over RSPI0...
[00:00:01.xxx,xxx] <inf> pixpaper_213m: Init done
[00:00:0x.xxx,xxx] <inf> pixpaper_213m: Cleared to white
[00:00:0x.xxx,xxx] <inf> pixpaper_213m: Displayed image (211x103)
```

The panel goes white first, then shows the image. Both steps are full refreshes
(the panel's own OTP waveform, about 2 s each); that is the slow but ghost-free
way to put a static image up. Sending the 4000-byte frame over RSPI0 takes about
20 ms at 4 MHz, so the time you see is the panel, not the bus.

## Displaying your own image

`src/png_HEX.h` holds the image as `img0[]` plus its `IMG_W` / `IMG_H`
dimensions. Each row is `(IMG_W + 31) / 32` `uint32` words, MSB-first across the
columns, where bit `1` = white. The image width maps to the panel gate axis and
the height to the source axis; the remainder of the panel is padded white.
Regenerate this header from a PNG and the app picks up the new dimensions
automatically.
