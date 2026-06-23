# pixpaper-213m — image (raw SPI)

Show a bundled image on the Open-EP pixpaper-213m e-paper panel.

This app maps `src/png_HEX.h` into the panel framebuffer and refreshes it: on
boot it initializes the panel, clears to white (to remove ghosting), renders the
image, and idles. E-paper keeps the image with no power.

Panel wiring, the devicetree binding, and how to install everything into a
Zephyr tree are in the [panel README](../../README.md). Platform prerequisites
(kernel clocks, the LPSPI3 node, remoteproc) are in the
[board README](../../../../README.md).

## Building

Needs a working Zephyr environment — `west`, the Zephyr SDK (toolchain) and the
Python dependencies; first-time setup is the official
[Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/).
With Zephyr set up, the patches applied (board README) and the panel files
installed (panel README):

```bash
cd /path/to/zephyrproject/zephyr
west build -p always -b frdm_imx93/mimx9352/m33 \
    samples/boards/nxp/frdm_imx93/pixpaper_213m/apps/image
```

The shared wiring overlay (`../../common/frdm_imx93_mimx9352_m33.overlay`) is
pulled in automatically by this app's `CMakeLists.txt`.

Then load `build/zephyr/zephyr.elf` from Linux via remoteproc (see the board
README). The M33 log on LPUART2 (115200 8N1):

```text
*** Booting Zephyr OS ... ***
[inf] pixpaper_213m: Initializing pixpaper-213m over LPSPI3...
[inf] pixpaper_213m: Init done
[inf] pixpaper_213m: Cleared to white
[inf] pixpaper_213m: Displayed image (211x103)
```

## Displaying your own image

`src/png_HEX.h` holds the image as `img0[]` plus its `IMG_W` / `IMG_H`
dimensions. Each row is `(IMG_W + 31) / 32` `uint32` words, MSB-first across the
columns, where bit `1` = white. The image width maps to the panel gate axis and
the height to the source axis; the remainder of the panel is padded white.
Regenerate this header from a PNG and the app picks up the new dimensions
automatically.

## Notes

This SoC's LPSPI corrupts any single SPI transfer longer than 10 bytes, so the
RAM data phase is sent in `EPD_CHUNK`-byte pieces (8 by default, which leaves
margin and is roughly 8× faster than one byte at a time). If you see a band of
wrong/garbage pixels in the middle of the image, lower `EPD_CHUNK` in
`src/main.c`.
