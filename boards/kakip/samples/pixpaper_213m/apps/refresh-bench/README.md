# pixpaper-213m: refresh-bench (how fast can the panel update?)

Measures the partial-refresh speed of the Open-EP pixpaper-213m from the Kakip
Cortex-M33. A black bar sweeps across the panel; for each waveform length (TP)
the app prints how long the SPI transfer takes, how long the panel is busy, and
the resulting frames per second.

Panel recipe is the one the W6300 video app uses: the full frame goes to 0x24
on every kick, 0x37 ping-pong is on, and 0x26 is never written after the base
frame.

## Building

```bash
cd ~/zephyrproject
.venv/bin/west build -p always -b rzv2h_evk/r9a09g057h44gbg/cm33 -d build-bench \
    path/to/this/repo/boards/kakip/samples/pixpaper_213m/apps/refresh-bench
```

Copy `build-bench/zephyr/zephyr.bin` to the SD card and start the M33 as in the
[board README](../../../../README.md). The log on SCI5:

```text
pixpaper-213m refresh benchmark: 24 frames per TP, SPI 4000000 Hz
  TP   spi_ms  busy_ms  frame_ms   fps
  0x10 ...
  0x0C ...
  ...
--- sweep done, repeating ---
```

- `spi_ms`: writing one 4000-byte frame over RSPI0
- `busy_ms`: the panel's own refresh time for that TP
- `frame_ms` / `fps`: the whole frame, which is the number you want

## Measured (2026-10-01, Kakip, 4 MHz SPI, FR = 3)

```text
  TP   spi_ms  busy_ms  frame_ms   fps
  0x10     22.4     279.2      303.0    3.2
  0x0C     22.4     226.0      249.8    4.0
  0x08     22.4     172.8      196.6    5.0
  0x06     22.4     146.2      170.0    5.8
  0x05     22.4     133.0      156.8    6.3
  0x04     22.4     119.6      143.5    6.9
  0x03     22.4     106.4      130.2    7.6
```

The panel time is linear: about 66 ms fixed plus 13.3 ms per TP unit. The SPI
time is 250 transfers of 16 bytes; sending the frame as one transfer would cut
it to about 9 ms. TP = 0x06 is the value the W6300 video app ships with.

## Reading the result

Each TP step starts with a full refresh (the clean base), then 24 partial
frames. Shorter TP = faster kicks but weaker drive. Watch the panel: the last TP
that still shows a clean black bar on a clean white background, with no grey
smear, is the usable speed limit for this panel on this bus. Below that the
numbers keep improving but the image does not.

The TP list is `TP_LIST[]` in `src/main.c`; `PARTIAL_FR` (frame-rate code) and
the SPI clock (`spi-max-frequency` in the shared overlay) are the other two
knobs.
