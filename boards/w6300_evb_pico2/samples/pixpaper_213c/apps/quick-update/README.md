# quick-update — image and colour bars

Alternates the bundled dithered image and a black/white/red/yellow bar test
pattern, 30 s apart, forever.

A four-colour refresh takes **15-25 s**, so after power-on be patient: the first
image takes a while to appear, and the bars are the easiest way to confirm all
four inks work.

## Build & flash

```bash
west build -b w6300_evb_pico2/rp2350a/m33 path/to/apps/quick-update
cp build/zephyr/zephyr.uf2 /media/$USER/RP2350/
```

Flash ~26 KB (1.3 %), RAM ~4 KB (0.8 %).

Console (optional, GP0/GP1 @ 115200) prints `drawing image`,
`refreshing (15-25 s)...`, `done` — useful for telling a slow refresh apart from
a hang.

The panel is parked (RST low) after each refresh, exactly as the Open-EP Linux
reference does, so the app re-runs the init sequence every cycle. That is
deliberate, not redundant.

Replace `src/img_packed.h` with your own 2bpp packed image — see the
[sample README](../../README.md) for the converter and dithering recipes.
