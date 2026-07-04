# console — boot image + interactive UART console

Draws the bundled image (`src/img_packed.h`) once at boot, puts the panel into
deep sleep, then serves a tiny command console on USART1 (115200 8N1,
TX=PD5/RX=PD6 — external 3.3V USB-UART dongle). Zephyr's shell subsystem does
not fit in 16KB; the console is a hand-rolled loop over the polled UART API.

## Build & flash

```bash
# in the west workspace (PR branch, see the board README)
west build -b uiapduino_pro_micro_ch32v003 path/to/apps/console
# enter the bootloader (hold reset, plug USB, release), then:
west flash        # or: minichlink -w build/zephyr/zephyr.bin flash
```

ROM ~14.8KB (90%), RAM ~1KB (50%).

## Console commands

| Command | Action |
| ------- | ------ |
| `draw` / `inv` | draw the bundled image (normal / inverted) |
| `white` / `black` | fill the panel |
| `load`  | receive a 4000-byte packed frame over UART and display it |
| `sleep` | panel deep sleep |
| `busy`  | read the BUSY pin (wiring diagnosis; idle panel = 0) |
| `ver`   | firmware version (also printed in the boot banner) |
| `help`  | list commands |

## Swapping the picture over UART (no reflash)

Convert the PNG once (needs python3 + opencv):

```bash
python3 ../../tools/png2bin.py picture.png     # -> picture.bin (4000 bytes)
```

Type `load` in the console, then send the `.bin` raw — TeraTerm
`File → Send file` (Binary checked), or from a second Linux shell
`cat picture.bin > /dev/ttyUSB0` while minicom stays open. The first byte may
take up to 60 s to arrive (time to click through dialogs); after that the
stream must keep flowing (3 s/byte timeout). The console prints one `.` per
250-byte chunk and `OK` when the panel has refreshed.

The image loaded this way lives in the panel's own RAM: a power cycle brings
back the bundled boot image. To change the bundled boot image, regenerate
`src/img_packed.h` with the `png2packed.py` converter from the
[arduino-user-space-examples](https://github.com/open-ep/arduino-user-space-examples)
repo (same packing format), then rebuild and reflash.
