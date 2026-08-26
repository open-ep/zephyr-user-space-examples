# pixpaper-213m e-paper (bit-banged SPI)

Drive an **Open-EP pixpaper-213m** 2.13" monochrome e-paper panel (250x122) on
the W6300-EVB-Pico2 / Raspberry Pi Pico 2 — no display subsystem and no SPI
driver: the panel is a slow write-only device, so SPI mode 0 is bit-banged over
plain GPIOs and pixels are generated or streamed straight from flash.

> Platform prerequisites — toolchain, Zephyr workspace, wiring and the
> drag-and-drop `.uf2` flashing flow — are in the
> [board README](../../README.md). Do those first.

This panel is an SSD1680-class controller: **BUSY idles LOW**, a full refresh
takes about 2 s, and partial refreshes take a few hundred ms.

## Apps

| App | What it does |
| --- | ------------ |
| [boot-image](apps/boot-image/) | Draw the bundled image once at boot, then put the panel into deep sleep. Start here. |
| [showcase](apps/showcase/) | Infinite scene loop, no input: self-playing Tetris, Snake and Pong, a static label image, and two 4-level grayscale images. |
| [video](apps/video/) | Play a pre-packed `.epdv` clip embedded in flash, looping forever at 5-6 fps. |
| [netvideo](apps/netvideo/) | Push frames over Ethernet: the board shows its DHCP address on the panel, then streams live from the PC or loops a clip uploaded into RAM. |

`tools/` holds the host-side helpers: `png2gray4.py` (PNG to the two grayscale
planes), `video2epd.py` (video or image folder to `.epdv`), `epdv2h.py`
(`.epdv` to a C header, since there is no filesystem on the board) and
`epdstream.py` (the sender for `netvideo` — video, GIF, webcam or screen to a
panel on the network).

`netvideo` is the only app here that needs anything beyond the panel: it drives
the board's W6300 ethernet, and until [PR #117112](https://github.com/zephyrproject-rtos/zephyr/pull/117112)
lands it needs the two driver fixes in
[`../../patches/zephyr/`](../../patches/zephyr/). Its README explains both.

## The two partial-update disciplines

Everything interesting about this panel comes down to one thing: the controller
holds **two RAM planes** — `0x24` (new frame) and `0x26` (what it believes is on
the glass) — and a partial refresh only drives pixels where they differ. There
are exactly two ways to keep those planes honest, and mixing them produces
solid "zombie" artifacts:

| | Full-frame writers (`video`) | Partial-area writers (`showcase` games) |
| --- | --- | --- |
| What you write per kick | the whole 4000 B frame | only the columns that changed |
| `0x37` byte 5 | `0x40` (ping-pong ON) — the controller alternates banks and diffs for you | **all zeros** (ping-pong OFF) — otherwise stale content resurfaces one kick later |
| `0x26` | never touched | re-synced to the same region after every kick |

Both disciplines also wait for BUSY **rise-then-fall** after the `0x20` kick.
Polling only for LOW races the few-ms assertion delay and truncates the
waveform, which looks exactly like weak ink.

A periodic full refresh (`0xF7`, OTP waveform) writes both planes and pays off
accumulated ghosting; the apps do this on scene entry or once per clip loop.

## 4-level grayscale

`showcase` also drives 4 grays out of a 1-bit panel: the two RAM planes are read
as a 2-bit-per-pixel code and a custom LUT gives each code its own waveform —
black = `(1,1)`, dark = `(0,1)`, light = `(1,0)`, white = `(0,0)` across
`(0x24, 0x26)`. The waveform and pixel coding come from the Open-EP Linux
reference (`pixpaper-213-m-test-frdm-imx93.c`); it is tuned per panel, so start
from that table rather than inventing one — wrong voltage polarity can shorten
panel life without looking obviously wrong.

Convert your own image with:

```bash
python3 tools/png2gray4.py photo.png src/img_gray4_test.h img_gray4_test
```

Add `--rotccw` for portrait sources and `--fit` to letterbox instead of
stretching; the script also writes a `_preview.png` so you can see the
quantization before flashing.
