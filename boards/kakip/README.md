# Kakip (Renesas RZ/V2H)

[Kakip](https://www.kakip.ai/) is a Renesas **RZ/V2H** board. One chip has
three kinds of cores:

| Core | Count | Runs here |
| ---- | ----- | --------- |
| Cortex-A55 | 4 | U-Boot (and normally Linux) |
| Cortex-M33 | 1, 200 MHz | Zephyr |
| Cortex-R8 | 2, 800 MHz | Zephyr on core 0 |

The samples here run **two Zephyr firmwares at the same time**, one on the
Cortex-M33 and one on Cortex-R8 core 0. Both are started from the U-Boot prompt
on the A55. Linux is not involved: the A55 stays at the U-Boot prompt.

Zephyr has no Kakip board, so the samples build for the upstream **RZ/V2H EVK**
targets and add a small Kakip overlay. Each app's `CMakeLists.txt` pulls in the
overlay automatically, so you only pass the board name:

| Core | Board target |
| ---- | ------------ |
| Cortex-M33 | `rzv2h_evk/r9a09g057h44gbg/cm33` |
| Cortex-R8 core 0 | `rzv2h_evk/r9a09g057h44gbg/cr8_0` |

Tested with Zephyr main `25179831f40` (2026-07-09), `hal_renesas` `29d0d3b` and
`cmsis` `512cc7e`, on a Kakip whose boot chain is TF-A 2.10 (`BOARD=kakip_1`) +
U-Boot 2024.07.

## Setup

### 1. Toolchain (one-time per machine)

Zephyr SDK **>= 1.0** with the `arm-zephyr-eabi` toolchain (both cores use it):

```bash
cd ~
wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v1.0.1/zephyr-sdk-1.0.1_linux-x86_64_minimal.tar.xz
tar xf zephyr-sdk-1.0.1_linux-x86_64_minimal.tar.xz
cd zephyr-sdk-1.0.1
./setup.sh -t arm-zephyr-eabi -c   # -c registers it so west finds it automatically
```

You also need `cmake`, `ninja-build`, `device-tree-compiler` and `python3-venv`.
If more than one Zephyr SDK is installed, point the build at this one:

```bash
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-1.0.1
```

### 2. Zephyr workspace

```bash
mkdir ~/zephyrproject && cd ~/zephyrproject
python3 -m venv .venv
.venv/bin/pip install west
.venv/bin/west init .
.venv/bin/west update hal_renesas cmsis cmsis_6   # only the modules these cores need
                                                  # (re-run after any `git -C zephyr pull`)
.venv/bin/pip install -r zephyr/scripts/requirements-base.txt
```

`hal_renesas` is the Renesas driver layer. `cmsis_6` is needed by the
Cortex-M33 and `cmsis` (CMSIS 5) by the Cortex-R8. If the R8 build fails with
`cmsis_core.h: No such file or directory`, the `cmsis` module is missing.

### 3. Build

See the sample: [dual_core_hello](samples/dual_core_hello/).

## Wiring

The Cortex-M33 console is **SCI5** on the 40-pin header **CN6**. Use a
**3.3 V** USB-UART adapter, 115200 8N1:

| CN6 pin | Signal | Connect to |
| ------- | ------ | ---------- |
| 6  | GND | adapter GND |
| 8  | P7_2, SCI5 TXD | adapter RXD |
| 10 | P7_3, SCI5 RXD | adapter TXD |

The Cortex-R8 has no UART of its own in these samples. Its output reaches the
same terminal through the M33 (see the sample README).

The A55 U-Boot console is the board's usual debug UART, on a second terminal.

## Starting the cores from U-Boot

### 1. Copy the firmware to the SD card

Copy the three files from the build (see the sample README for where they are)
to the **root of the SD card's first partition** (FAT, `mmc 0:1` in U-Boot).
U-Boot's `ls mmc 0:1` should list them.

### 2. Stop at the U-Boot prompt and paste one line per core

Paste each whole line at the `=>` prompt, **CM33 first, then CR8**.

**Cortex-M33**:

```
dcache off; mw.l 0x10420D2C 0x02000000; mw.l 0x1043080c 0x08003000; mw.l 0x10430810 0x18003000; mw.l 0x10420604 0x00040004; mw.l 0x10420C1C 0x00003100; mw.l 0x10420C0C 0x00000001; mw.l 0x10420904 0x00380008; mw.l 0x10420904 0x00380038; fatload mmc 0:1 0x58000000 kakip_cm33.bin; cp.b 0x58000000 0x08003000 ${filesize}; mw.l 0x10420C0C 0x00000000; dcache on
```

**Cortex-R8 core 0**:

```
dcache off; mw.l 0x10420D24 0x04000000; mw.l 0x10420600 0xE000E000; mw.l 0x10420604 0x00030003; mw.l 0x10420908 0x1FFF0000; mw.l 0x10420C44 0x003F0000; mw.l 0x10420C14 0x00000000; mw.l 0x10420908 0x10001000; mw.l 0x10420C48 0x00000020; mw.l 0x10420908 0x1FFF1FFF; mw.l 0x10420C48 0x00000000; fatload mmc 0:1 0x58000000 kakip_cr8_itcm.bin; cp.b 0x58000000 0x12040000 ${filesize}; fatload mmc 0:1 0x58100000 kakip_cr8_sram.bin; cp.b 0x58100000 0x08180000 ${filesize}; mw.l 0x10420C14 0x00000003; dcache on
```

U-Boot should report the file sizes as it loads them (e.g. `21976 bytes read`
for the M33, then `60 bytes` and `32844 bytes` for the R8). The `mw.l` writes
are the clock and reset sequence from Renesas and Kakip. Each line holds the core
in reset, copies its image in, then releases the core.

Pasting the same line again restarts that core with a fresh copy. That is how you
reload after a rebuild, without power-cycling the board.

### Why each image is loaded through DDR

U-Boot 2024.07 refuses to `fatload` directly into SRAM:

```
** Reading file would overwrite reserved memory **
```

Its memory map only covers DDR. So each line loads the file into DDR at
`0x58000000` (U-Boot's default load address) and then `cp.b` copies it into
SRAM. `cp` has no such check.

### Where the images go

| Image | Address (A55 view) | Region |
| ----- | ------------------ | ------ |
| `kakip_cm33.bin` | `0x0800_3000` | MSRAM, M33 code and data (the M33 vector table address) |
| `kakip_cr8_itcm.bin` | `0x1204_0000` | CR8 core 0 ITCM, the 60-byte vector table (CR8 address 0) |
| `kakip_cr8_sram.bin` | `0x0818_0000` | R8SRAM, CR8 code and data |
| (shared log ring) | `0x081F_F000` | last 4 KB of R8SRAM, written by the R8, read by the M33 |

Zephyr images start with the vector table, so the M33 image goes to
`0x08003000` exactly. Kakip's FSP examples load their `.bin` at `0x08001e00`
instead, because their images carry a 0x1200-byte header in front of the vector
table.

## If nothing shows up

- **No output at all on CN6.** Check the adapter is 3.3 V and TX/RX are crossed.
  Then run `md.l 0x08003000 4` in U-Boot. If the M33 image is in place, the first
  word is the stack pointer (`0x0800....`) and the second is the reset handler.
- **`md.l` shows zeros or garbage after the copy.** The boot firmware is not
  letting the A55 write that SRAM. On Kakip, TF-A has to open the M33 and R8 SRAM
  to the non-secure world; the Kakip TF-A port (`BOARD=kakip_1`) does.
- **The M33 prints, but no `[cr8]` lines ever appear.** Run `md.l 0x081FF000 4`.
  The first word should be `43523853` (the ring's magic) and the second (bytes
  written) should keep growing. If both hold, the R8 is running and the problem is
  on the M33 side. If not, the R8 did not start.

## Booting Linux afterwards

Not covered by these samples. If you continue to `boot` Linux, the kernel turns
off clocks that no Linux driver uses, including SCI5's. The M33 keeps running but
its UART goes quiet. Keeping it alive needs a kernel change that marks the
`rsci5_*` clocks in `drivers/clk/renesas/r9a09g057-cpg.c` as critical. That is
the same idea as the FRDM-IMX93 clock patch in this repo.
