# boot-image — draw once, then sleep

Draws the bundled image (`src/img_packed.h`) with one full refresh, then puts
the panel into deep sleep. The image stays on the glass with no power at all,
so you can unplug the board and it keeps showing the picture.

Start with this app: it proves the wiring and the panel in about three seconds.

## Build & flash

```bash
west build -b w6300_evb_pico2/rp2350a/m33 path/to/apps/boot-image
# hold BOOTSEL, plug USB, release, then copy the .uf2 onto the RP2350 drive:
cp build/zephyr/zephyr.uf2 /media/$USER/RP2350/
```

Flash ~22 KB (1.1 %), RAM ~4 KB (0.8 %).

Console (optional, GP0/GP1 @ 115200) prints `drawing image` then `done`. Without
a panel connected you get two `warn: BUSY timeout` lines and it still finishes —
handy for checking the firmware runs before wiring anything.

The image is written to **both** RAM planes before the refresh so the panel
starts from a consistent reference. Replace it by regenerating `img_packed.h`
with the `png2packed.py` converter from the
[arduino-user-space-examples](https://github.com/open-ep/arduino-user-space-examples)
repo (same 1bpp column-major packing).
