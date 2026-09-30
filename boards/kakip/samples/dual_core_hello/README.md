# dual_core_hello: two Zephyr kernels, one terminal

Run Zephyr on the Kakip's **Cortex-M33** and **Cortex-R8 core 0** at the same
time. Each core prints `Hello World` and then a `tick` once a second, and both
show up on one UART:

```text
[cm33] Hello World from Zephyr on Kakip CM33 (rzv2h_evk/r9a09g057h44gbg/cm33)
[cm33] relaying CR8 log from 0x081ff000
[cm33] tick 0
[cm33] tick 1
[cr8]  *** Booting Zephyr OS build ... ***
[cr8]  Hello World from Zephyr on Kakip CR8_0 (rzv2h_evk/r9a09g057h44gbg/cr8_0)
[cr8]  tick 0
[cm33] tick 2
[cr8]  tick 1
...
```

> Setup (SDK, `west update hal_renesas cmsis cmsis_6`), wiring, and the U-Boot
> lines that start each core are in the [board README](../../README.md).

## Apps

| App | Core | What it does |
| --- | ---- | ------------ |
| [cm33](apps/cm33/) | Cortex-M33 | Prints its own ticks on SCI5, and copies the R8's log to the same UART, tagged `[cr8]`. |
| [cr8](apps/cr8/) | Cortex-R8 core 0 | Has no UART. Its `printk` output goes into a ring buffer in shared SRAM. |

`common/` holds what both apps share: the ring layout (`shlog.h`) and one
overlay per core.

## How the R8 borrows the M33's UART

```
 Cortex-R8                    R8SRAM, last 4 KB              Cortex-M33
 printk() --> hook --> [ magic | head | boots | buf[4080] ] <-- polls every 50 ms --> SCI5 (CN6)
                        0x081F_F000
```

- **Where the ring lives.** The R8 overlay shrinks the R8's RAM region from 512 KB
  to 508 KB. The last 4 KB (`0x081FF000`) is then never used by the R8 image, and
  both cores reach it at the same address.
- **Writer, CR8.** A `printk` hook, installed before the boot banner, writes each
  character at `buf[head % size]` and then increments `head`. The R8 has a data
  cache and the M33 does not, so every byte and every `head` update is flushed out
  of the cache, with a memory barrier in between. Otherwise the M33 would read
  stale SRAM.
- **Reader, CM33.** Every 50 ms it prints everything between the last `head` it
  saw and the current one, adding `[cr8]` at the start of each line. `boots`
  changes on every R8 start, so if you restart only the R8, the M33 starts over
  from the beginning of the new log.
- **It is lossy on purpose.** If the M33 falls more than 4 KB behind, it skips to
  the newest data instead of printing overwritten bytes.

## Build

From your Zephyr workspace (see the board README), build each app into its own
build directory:

```bash
cd ~/zephyrproject
HERE=path/to/this/repo/boards/kakip/samples/dual_core_hello

.venv/bin/west build -p always -b rzv2h_evk/r9a09g057h44gbg/cm33  -d build-cm33 $HERE/apps/cm33
.venv/bin/west build -p always -b rzv2h_evk/r9a09g057h44gbg/cr8_0 -d build-cr8  $HERE/apps/cr8
```

This gives three files. Copy them to the SD card's first partition under these
names (the names the U-Boot lines in the board README use):

| Build output | Copy to the SD card as | Size |
| ------------ | ---------------------- | ---- |
| `build-cm33/zephyr/zephyr.bin` | `kakip_cm33.bin` | ~22 KB |
| `build-cr8/zephyr/cr8_itcm.bin` | `kakip_cr8_itcm.bin` | 60 B |
| `build-cr8/zephyr/cr8_sram.bin` | `kakip_cr8_sram.bin` | ~32 KB |

For example, with the SD card mounted at `/mnt`:

```bash
sudo cp build-cm33/zephyr/zephyr.bin    /mnt/kakip_cm33.bin
sudo cp build-cr8/zephyr/cr8_itcm.bin   /mnt/kakip_cr8_itcm.bin
sudo cp build-cr8/zephyr/cr8_sram.bin   /mnt/kakip_cr8_sram.bin
sync
```

The R8 build has no `zephyr.bin`, because that file would be about 135 MB. The
R8 image has two load regions: the vector table at CR8 address 0 (ITCM) and
everything else in R8SRAM at `0x08180000`. A single raw binary covering both
would be padded across the whole gap. The R8 app's `CMakeLists.txt` writes the
two regions as separate files instead.

Then start both cores from U-Boot as described in the
[board README](../../README.md#starting-the-cores-from-u-boot).

## Memory used

| Core | Region | Used |
| ---- | ------ | ---- |
| CM33 | MSRAM, ~1 MB | ~28 KB |
| CR8 | R8SRAM, 508 KB (plus the 4 KB ring) | ~40 KB |

So there is plenty of room left for a real application on either core.
