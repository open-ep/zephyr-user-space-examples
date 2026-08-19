# showcase — six scenes, no input needed

An infinite loop that exercises everything this panel can do. Each scene runs
30 s with a big title card in between:

| Scene | What it shows |
| ----- | ------------- |
| TETRIS | Tetris playing itself (heuristic AI: lines good, height/holes/bumpiness bad) |
| SNAKE | Snake playing itself (greedy toward food, flood-fill check so it does not trap itself) |
| PONG | Both paddles tracked by AI at slightly different speeds, on an inverted black court |
| ESL | A static shelf-label style image |
| GRAY-4 | A 4-level grayscale test pattern |
| NAGOYA | A 4-level grayscale photograph |

Nothing to connect but the panel — no console, no keyboard.

## Build & flash

```bash
west build -b w6300_evb_pico2/rp2350a/m33 path/to/apps/showcase
cp build/zephyr/zephyr.uf2 /media/$USER/RP2350/
```

Flash ~43 KB (2.1 %), RAM ~5 KB (1.0 %). One full loop takes about 3 minutes.

## How the games stay clean

There is no framebuffer — 2 KB would not even hold one frame, and this code was
first written for a CH32V003. Every scene is a **procedural renderer**: a
function that returns one byte of the display for a given column, computed on
the fly. The Tetris well is a 20-entry bitmask array, Snake's body is a
2-bit-per-cell direction map (105 B) plus an occupancy bitmap (53 B).

Each tick only the changed cells are rewritten, using the partial-area
discipline described in the [sample README](../../README.md): `0x37` all zeros,
BUSY rise-then-fall, `0x26` re-synced after every kick. Scene entry does a full
refresh to clear accumulated ghosting.

Speed knob: `PHASE0_TP` (phase-0 length of the partial waveform). `0x0C` here;
lower is faster with weaker drive, higher is slower and cleaner.

## Swapping the grayscale images

```bash
python3 ../../tools/png2gray4.py photo.png src/img_gray4_test.h img_gray4_test
```

Then rebuild. Add `--rotccw` for portrait sources, `--fit` to letterbox rather
than stretch. Use your own images: the two shipped here are a generated test
pattern and an Open-EP photo.
