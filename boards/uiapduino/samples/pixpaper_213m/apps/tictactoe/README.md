# tictactoe — cursor-driven game with a full-refresh-per-move design

Tic-tac-toe against the computer (you are O, it answers with X), played over
USART1 (115200 8N1, TX=PD5/RX=PD6 — external 3.3V USB-UART dongle).

## Controls

| Key | Action |
| --- | ------ |
| `w` `a` `s` `d` | move the selection cursor |
| `Enter` | place your O (the computer answers immediately) |
| any key | new game after win / loss / draw |

## Build & flash

```bash
# in the west workspace (PR branch, see the board README)
west build -b uiapduino_pro_micro_ch32v003 path/to/apps/tictactoe
# enter the bootloader (hold reset, plug USB, release), then:
west flash        # or: minichlink -w build/zephyr/zephyr.bin flash
```

ROM ~12.3KB (75%), RAM ~1.0KB (51%).

## Display strategy

This app plays to the panel's strengths instead of fighting them:

- **Pieces are drawn once and never erased** — placed moves cannot ghost.
- The only transient element is the thin **cursor frame**, moved with fast
  partial updates (write changed columns to `0x24` → kick → sync `0x26`).
- **Every placed move triggers one full refresh** — a natural
  "move confirmed" flash that also wipes any accumulated cursor ghosting.
- Idle = zero refreshes.

The partial-update rules shared by all game demos here (0x37 all zeros,
BUSY rise-then-fall, post-kick `0x26` sync) are documented in the
[snake README](../snake/README.md); this game is where they were first
hardware-proven.

The computer plays a simple win → block → preferred-cell policy
(`computer_move()` in `src/main.c`) — beatable, but it punishes blunders.
