# tetris — self-playing Tetris in portrait

Tetris on the panel held upright (10-wide × 20-deep well, gravity along the
gate axis), played by a built-in AI with no input at all: plug in power and it
plays, tops out, pauses, and starts over. No UART is compiled in — of the
game demos here this is the only one that runs fully unattended, so it doubles
as a shop-window / magazine-shot demo.

## What the AI does

For every new piece it tries all 4 rotations × every column, drops each one
straight down and scores the resulting board with the classic four-feature
linear heuristic (Yiyuan Lee's weights, integer-scaled ×100):

| Feature | Weight | Meaning |
| ------- | -----: | ------- |
| lines cleared | +76 | reward a clear |
| aggregate height | −51 | sum of column heights, keep the stack low |
| holes | −36 | empty cells with something above them |
| bumpiness | −18 | sum of height differences between neighbour columns |

One-piece lookahead only (the next piece is ignored); on a host simulation it
clears anywhere from a few dozen to over a thousand lines per game
before topping out, depending on the piece sequence. Rotation and shifting happen
at the spawn row before the piece falls, so what it plans is what it plays.

## Build & flash

```bash
# in the west workspace (PR branch, see the board README)
west build -b uiapduino_pro_micro_ch32v003 path/to/apps/tetris
# enter the bootloader (hold reset, plug USB, release), then:
west flash        # or: minichlink -w build/zephyr/zephyr.bin flash
```

ROM 11160 B (68.1%), RAM 1056 B (51.6%) — Zephyr SDK 1.0.1, `-Os`, no LTO.

## Display strategy

Same anti-ghosting discipline as the other game demos (see the
[snake README](../snake/README.md) for the register-level explanation):
`0x37` all zeros, BUSY waited rise-then-fall, and the reference plane `0x26`
re-synced after every kick. The panel-level code is a verbatim copy of
[pong](../pong/src/main.c) (only the ghost-refresh counter reset moved into
`rebase()`).

There is no framebuffer: the well frame and every cell are generated
procedurally per gate column from a 20 × `uint16_t` occupancy bitmap
(40 bytes), and each move rewrites only the well rows the piece spanned
before and after. Every line clear runs one full refresh — the flash is the
feedback and pays off accumulated ghosting; `GHOST_EVERY` (partial kicks
between forced rebases) covers long stretches without a clear.

Knobs in `src/main.c`: `ROT_180` (panel held the other way up),
`TICK_MS`, `GAMEOVER_MS`, `GHOST_EVERY`.
