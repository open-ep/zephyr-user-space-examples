# UIAPduino Pro Micro CH32V003

A tiny RISC-V board ([UIAP UIAPduino Pro Micro CH32V003 V1.4](https://www.uiap.jp/en/uiapduino/pro-micro/ch32v003/v1dot4)):
WCH CH32V003 @ 48 MHz, **16KB flash / 2KB SRAM**, Pro Micro form factor,
flashed over USB Type-C through its rv003usb bootloader — no external
programmer needed. Unlike the remoteproc boards in this repo, this is a
standalone MCU: the Zephyr firmware IS the whole system.

## Setup from scratch

Two separate downloads are involved — don't confuse them:

- the **Zephyr source tree** (kernel, drivers — the code that gets compiled), and
- the **Zephyr SDK** (the RISC-V cross-toolchain that compiles it).

### 1. Toolchain (one-time per machine)

Your host gcc targets x86; the board needs `riscv64-zephyr-elf-gcc` from the
Zephyr SDK, **version >= 1.0** (0.17.x is rejected by this tree). The minimal
tarball plus one toolchain is a few hundred MB instead of several GB:

```bash
cd ~
wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v1.0.1/zephyr-sdk-1.0.1_linux-x86_64_minimal.tar.xz
tar xf zephyr-sdk-1.0.1_linux-x86_64_minimal.tar.xz
cd zephyr-sdk-1.0.1
./setup.sh -t riscv64-zephyr-elf -c   # -c registers it in ~/.cmake so west finds it automatically
```

You will also need the usual build tools: `cmake`, `ninja-build`,
`device-tree-compiler`, `python3-venv` (e.g. via `apt install`).

### 2. Zephyr workspace

Upstream board support is in review
([zephyrproject-rtos/zephyr#112278](https://github.com/zephyrproject-rtos/zephyr/pull/112278),
board `uiapduino_pro_micro_ch32v003`). Until it merges, use the PR branch
directly — **this is what every sample here was developed and
hardware-verified on**:

```bash
mkdir ~/zephyrproject && cd ~/zephyrproject
python3 -m venv .venv
.venv/bin/pip install west

# fetch the Zephyr source (PR branch from the author's fork)
.venv/bin/west init -m https://github.com/yashi/zephyr --mr add-uiap-pro-micro .

# pin the commit these samples were verified against:
git -C zephyr checkout 25179831f40

# fetch ONLY the WCH register-definition module (a bare `west update`
# would download every vendor HAL — several GB you don't need)
.venv/bin/west update hal_wch

# python packages Zephyr's build scripts need (devicetree/Kconfig tooling)
.venv/bin/pip install -r zephyr/scripts/requirements-base.txt
```

If the `git checkout` fails with `pathspec ... did not match`, the PR branch
has been rebased again and the pinned commit is no longer fetchable — build
against the current branch tip instead (`git -C zephyr log --oneline -1` to
record which), and expect a slightly different binary than documented.

Once the PR merges upstream, switch the manifest back to
`zephyrproject-rtos/zephyr` `main` — the board name stays the same, so the
apps need no changes.

### 3. Build a sample

```bash
cd ~/zephyrproject
.venv/bin/west build -b uiapduino_pro_micro_ch32v003 path/to/this/repo/boards/uiapduino/samples/pixpaper_213m/apps/snake
```

The firmware lands in `build/zephyr/zephyr.bin` (note the extra `/zephyr/`
path segment). Flashing: see below.

## Hardware prerequisite: 3.3V rework

**The board ships at 5V logic and must be reworked to 3.3V before connecting a
PIXPAPER panel.** At 5V the GPIOs over-drive the 3.3V panel (faint image,
back-feed into the 3.3V rail). Cut the 5V side of the Volt-Sel solder jumper
(minimal cut - traces run right next to it), bridge the 3.3V side, and verify
with a multimeter that the 5V side is open. See the board page for the jumper
location.

## Flashing

The bootloader enumerates as a USB HID device (VID:PID `1209:b803`) and is
driven by `minichlink` from [cnlohr/ch32fun](https://github.com/cnlohr/ch32fun):

1. Enter the bootloader: unplug USB, hold the reset button, plug in, release.
2. `west flash`, or directly: `minichlink -w build/zephyr/zephyr.bin flash`
3. `Image written.` = done. Power-cycle to run.

Notes:

- The samples here have no USB function, so the board disappears from USB once
  the app runs - repeat the reset ritual for every re-flash.
- The bootloader's software USB is timing-sensitive: if enumeration fails
  (unknown device / `error -71` in dmesg), go through a **USB 2.0 hub** instead
  of a USB 3.0 port.
- The bootloader window is short; a retry loop helps:
  `while true; do minichlink -w zephyr.bin flash && break; sleep 0.3; done`

## Known limitations

- `CONFIG_LTO` cannot be enabled on CH32V003 (Kconfig dependency chain needs
  `GEN_IRQ_VECTOR_TABLE`, which the qingke-v2a port lacks). Fit code by
  Kconfig pruning instead (`CONFIG_MULTITHREADING=n` saves ~2.6KB).
- 16KB flash fits the mono panel sample comfortably; the 4-color PIXPAPER
  panel (8KB image) does not fit alongside Zephyr on this MCU.

## Samples

| Sample | Panel |
| ------ | ----- |
| [pixpaper_213m](samples/pixpaper_213m/) | Open-EP pixpaper-213m 2.13" mono, bit-banged SPI |
