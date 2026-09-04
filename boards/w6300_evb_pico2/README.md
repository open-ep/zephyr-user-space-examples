# WIZnet W6300-EVB-Pico2 (RP2350)

A [W6300-EVB-Pico2](https://docs.wiznet.io/Product/iEthernet/W6300/w6300-evb-pico2)
is a Raspberry Pi Pico 2 form-factor board: **RP2350** (dual Cortex-M33 @ 150 MHz,
520 KB SRAM, 2 MB flash) plus a W6300 hardwired-TCP/IP ethernet chip. Like the
UIAPduino, this is a standalone MCU — the Zephyr firmware is the whole system.

Board support is **upstream** (`w6300_evb_pico2`), so no PR branch or patches are
needed. Most of the firmware here also builds for the plain
**Raspberry Pi Pico 2** (`rpi_pico2/rp2350a/m33`) and produces a byte-identical
binary, because those samples only use GP2–GP7 and never touch the W6300.

The one exception is [netvideo](samples/pixpaper_213m/apps/netvideo/), which
drives the W6300 to receive frames over Ethernet — that one needs this board,
and two driver fixes listed in its README.

## Why this board is comfortable

| | UIAPduino (CH32V003) | This board (RP2350) |
| --- | --- | --- |
| Flash / SRAM | 16 KB / 2 KB | 2 MB / 520 KB |
| 4-colour panel (7750 B image) | does not fit | 1.3 % of flash |
| Logic level | needs the 3.3 V Volt-Sel rework | natively 3.3 V, no rework |
| Flashing | USB bootloader + `minichlink` ritual | hold BOOTSEL, drag a `.uf2` |

The extra room is what makes the bigger samples here possible: a 4-level
grayscale image, a self-playing games showcase, and a video player with the
clip embedded in flash.

## Setup

### 1. Toolchain (one-time per machine)

Zephyr SDK **>= 1.0** with the `arm-zephyr-eabi` toolchain. The minimal tarball
plus one toolchain is a few hundred MB instead of several GB:

```bash
cd ~
wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v1.0.1/zephyr-sdk-1.0.1_linux-x86_64_minimal.tar.xz
tar xf zephyr-sdk-1.0.1_linux-x86_64_minimal.tar.xz
cd zephyr-sdk-1.0.1
./setup.sh -t arm-zephyr-eabi -c   # -c registers it so west finds it automatically
```

You also need `cmake`, `ninja-build`, `device-tree-compiler` and `python3-venv`.

If more than one Zephyr SDK is registered on your machine (for example a
RISC-V-only install for another board), CMake may pick one that lacks the
`arm-zephyr-eabi` toolchain and fail with `arm-zephyr-eabi-gcc not found`. Point the build at
the right SDK explicitly, then rebuild pristine (`-p always`):

```bash
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-1.0.1   # the SDK that has arm-zephyr-eabi
```

### 2. Zephyr workspace

```bash
mkdir ~/zephyrproject && cd ~/zephyrproject
python3 -m venv .venv
.venv/bin/pip install west
.venv/bin/west init .
.venv/bin/west update hal_rpi_pico cmsis_6      # only the modules this board needs
.venv/bin/pip install -r zephyr/scripts/requirements-base.txt
```

### 3. Build

```bash
cd ~/zephyrproject
.venv/bin/west build -b w6300_evb_pico2/rp2350a/m33 \
    path/to/this/repo/boards/w6300_evb_pico2/samples/pixpaper_213m/apps/boot-image
```

The firmware lands in `build/zephyr/zephyr.uf2` (note the extra `/zephyr/` path
segment). For the official Pico 2, swap the board for `rpi_pico2/rp2350a/m33`.

## Flashing

1. Unplug USB, hold **BOOTSEL**, plug USB back in, release — a USB mass-storage
   drive named `RP2350` appears.
2. Copy `build/zephyr/zephyr.uf2` onto it.
3. The board unmounts itself and reboots into the new firmware immediately.

No driver, no flashing tool, no reset ritual. The bootloader lives in ROM, so it
is always reachable and cannot be bricked.

Once your firmware runs, the board no longer appears as a USB device (these
samples have no USB function) — that is expected, not a failed flash.

## Wiring (all samples)

| Panel | GPIO | Header pin | Notes |
| ----- | ---- | ---------- | ----- |
| VCC   | 3V3  | 36 | panel is 3.3 V-only |
| GND   | GND  | 8  | |
| CLK   | GP2  | 4  | bit-banged SCK, mode 0 |
| DIN   | GP3  | 5  | bit-banged MOSI |
| CS    | GP4  | 6  | |
| DC    | GP5  | 7  | low = command, high = data |
| RST   | GP6  | 9  | low = reset |
| BUSY  | GP7  | 10 | polarity differs per panel — see each sample |

GP2–GP7 are header pins 4–10 in one run, so the panel ribbon lands on a single
edge. They were chosen to stay clear of the W6300 (GP15–GP22) and of the console
UART (GP0/GP1), which keeps the same wiring valid on a bare Pico 2.

Console output (`printk`) is on UART0: TX = GP0, RX = GP1, 115200 8N1, via a
3.3 V USB-UART dongle. It is optional — every sample runs without it.

SPI is bit-banged over plain GPIOs on purpose: these panels are slow,
write-only devices, so a hardware SPI controller buys nothing and the code stays
identical across boards.

## Zephyr patches

[`patches/zephyr/`](patches/zephyr/) holds two `eth_w6300` driver fixes needed
by the `netvideo` sample: without them the chip's TX path wedges under
sustained traffic and the board drops off the network until it is power-cycled.
They are submitted upstream as
[zephyrproject-rtos/zephyr#117112](https://github.com/zephyrproject-rtos/zephyr/pull/117112)
(open, awaiting review as of August 2026) — once that merges, skip them. The
copies here are pinned and work offline; `curl -L
https://github.com/zephyrproject-rtos/zephyr/pull/117112.patch | git apply`
fetches the current version instead. Nothing else in this repo needs a patched
Zephyr.

## Samples

| Sample | Panel |
| ------ | ----- |
| [pixpaper_213m](samples/pixpaper_213m/) | Open-EP pixpaper-213m 2.13" mono (boot image, games showcase, offline video player, Ethernet frame player) |
| [pixpaper_213c](samples/pixpaper_213c/) | Open-EP pixpaper-213c 2.13" 4-colour (image + colour bars) |
