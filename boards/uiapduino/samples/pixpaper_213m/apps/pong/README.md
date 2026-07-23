# pong — the stress test: a ball sweeping the screen every frame

Paddle-ball on an inverted (black) court, played over USART1 (115200 8N1,
TX=PD5/RX=PD6 — external 3.3V USB-UART dongle). Of the game demos here this
is the most demanding one for the panel: unlike the snake (one cell moves per
tick) or tic-tac-toe (only a cursor moves), the ball erases and redraws its
full trajectory span every single frame.

## Controls

| Key | Action |
| --- | ------ |
| `w` / `s` | left paddle up / down (you) |
| `i` / `k` | right paddle up / down (it also auto-tracks the ball) |
| any key | restart after game over |

Miss the ball with the left paddle and the game ends; the rally count is
printed on the console.

## Build & flash

```bash
# in the west workspace (PR branch, see the board README)
west build -b uiapduino_pro_micro_ch32v003 path/to/apps/pong
# enter the bootloader (hold reset, plug USB, release), then:
west flash        # or: minichlink -w build/zephyr/zephyr.bin flash
```

ROM ~11.8KB (72%), RAM ~1.0KB (51%).

## Display strategy

Same anti-ghosting discipline as the other game demos (see the
[snake README](../snake/README.md) for the register-level explanation):
`0x37` all zeros, BUSY waited rise-then-fall, and the reference plane `0x26`
re-synced after every kick.

Two knobs specific to this app (`src/main.c`):

- `INVERT_COURT` (default 1) — black court, white ball. Any residue this
  panel leaves is "dark gray on black", which the eye cannot see; set to 0
  for a classic white court if you want to inspect erase quality instead.
- `GHOST_EVERY` — frames between full-refresh rebases (the periodic flash
  that pays off accumulated ghosting debt).

There is no framebuffer: paddles, the dashed centre line and the ball are
generated procedurally per gate column, and only the columns that changed
each tick are rewritten.
