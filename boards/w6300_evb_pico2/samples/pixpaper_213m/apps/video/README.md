# video — play a clip from flash

Plays a pre-packed `.epdv` clip embedded in the firmware, looping forever at
roughly 5-6 fps. The bundled clip is a 12 s / 72-frame synthetic animation (bouncing ball
and a sweeping bar) — high-contrast silhouettes are what a 1-bit panel renders
best.

All the work happens on the host: `video2epd.py` decodes, scales, thresholds and
packs every frame **into controller RAM order**, so at play time the board only
pushes 4000 bytes per frame over SPI and kicks a refresh. Nothing is decoded on
the target.

## Build & flash

```bash
west build -b w6300_evb_pico2/rp2350a/m33 path/to/apps/video
cp build/zephyr/zephyr.uf2 /media/$USER/RP2350/
```

Flash ~300 KB (15 %) — 288 KB of that is the clip itself. RAM ~4 KB (0.8 %).

## Using your own clip

```bash
python3 ../../tools/video2epd.py your.mp4 -o clip.epdv --fps 6 \
        --fit crop --dither none --gamma 1.6 --preview sheet.png
python3 ../../tools/epdv2h.py clip.epdv src/clip_epdv.h
```

Then rebuild. Budget: 2 MB flash is about 470 frames, i.e. 78 s at 6 fps.

Look at `sheet.png` before flashing. Two things matter more than resolution
(the tool always scales to 250x122):

- **`--dither none` for pure black and white.** Ordered/Floyd-Steinberg dither
  looks better on a still image, but the dither pattern shimmers between frames
  and each partial refresh has to drive those speckles.
- **`--gamma` is your contrast knob** and it runs the counter-intuitive way here:
  values **above 1** brighten. Live-action footage usually needs 1.4-1.8 before
  a face reads as a silhouette.

## Frame rate

Real fps is set by the refresh, not the clip: writing 4000 B bit-banged takes
about 40 ms and the kick a few hundred, so `PHASE0_TP` in `src/main.c` is the
real throttle — `0x06` here as a compromise. Lower it toward `0x05` for speed at
the cost of trailing ghosts; raise it toward `0x10` for cleaner transitions.

The player prints one line per loop on the console (GP0/GP1 @ 115200) with the
measured per-frame time, so you can tune it with numbers instead of guessing.
