# snake — partial-update game demo

Classic snake on the e-paper, steered over USART1 (115200 8N1, TX=PD5/RX=PD6
— external 3.3V USB-UART dongle). A full refresh runs only at game
start/restart; every move is a fast partial update that rewrites just the
cells that changed (new head, vacated tail, eaten/respawned food).

## Controls

| Key | Action |
| --- | ------ |
| `w` `a` `s` `d` | steer (reversing into yourself is ignored) |
| any key | start / restart after game over |

Eat a food square to grow by 2. Hitting the wall or your own body ends the
game; the score (snake length) is printed on the console.

## Build & flash

```bash
# in the west workspace (PR branch, see the board README)
west build -b uiapduino_pro_micro_ch32v003 path/to/apps/snake
# enter the bootloader (hold reset, plug USB, release), then:
west flash        # or: minichlink -w build/zephyr/zephyr.bin flash
```

ROM ~12.2KB (74%), RAM ~1.2KB (59%).

## How the display is driven (why there is no ghost trail)

The panel controller keeps two RAM planes — `0x24` (new frame) and `0x26`
(reference = what it believes is on the glass) — and on each partial refresh
drives only the pixels where they differ. Three rules make moving objects
clean, all applied in `src/main.c`:

1. **`0x37` all zeros** — the `0x40` "ping-pong" option makes the `0x24`
   address alternate between two physical planes per refresh, which
   resurrects stale content when you write only changed cells.
2. **BUSY is waited rise-then-fall** — BUSY takes a few ms to assert after
   the `0x20` kick; polling for low immediately truncates the waveform.
3. **`0x26` is re-synced after every kick** — otherwise erases run the weak
   white→white waveform and old segments only fade slowly.

Game speed is refresh-bound. The knob is `PHASE0_TP` (phase-0 length in
frames of the partial LUT): `0x08` here for a fast game; raise toward `0x10`
if erases look weak on your panel.

## Memory tricks (2KB SRAM)

No framebuffer: the scene is computed per gate column on the fly. The snake
body is stored as a 2-bit-per-cell direction map (105 B) plus an occupancy
bitmap (53 B) over the 30×14 grid — the tail follows the head by walking the
direction map, so the body can grow to fill the whole field.
