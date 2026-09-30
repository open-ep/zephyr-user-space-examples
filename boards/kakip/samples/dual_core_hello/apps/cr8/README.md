# dual_core_hello: CR8 (printk into shared SRAM)

Zephyr on the Kakip's Cortex-R8 core 0. It prints `Hello World` and a `tick`
every second. It has no UART of its own: a `printk` hook writes the characters
into a ring buffer in the last 4 KB of R8SRAM, and the
[CM33 app](../cm33/) prints them.

How the ring works is explained in the [sample README](../../README.md). Setup
and the U-Boot start line are in the [board README](../../../../README.md).

## Building

```bash
cd ~/zephyrproject
.venv/bin/west build -p always -b rzv2h_evk/r9a09g057h44gbg/cr8_0 -d build-cr8 \
    path/to/this/repo/boards/kakip/samples/dual_core_hello/apps/cr8
```

The overlay (`../../common/kakip_cr8.overlay`) is pulled in automatically. It
shrinks the R8's RAM to 508 KB, which keeps the ring's 4 KB free, and it turns
off the EVK console UART.

Output: two files, because the image has two load regions:

| File | Copy to the SD card as | Loaded at (A55 view) |
| ---- | ---------------------- | -------------------- |
| `build-cr8/zephyr/cr8_itcm.bin` | `kakip_cr8_itcm.bin` | `0x12040000`, CR8 ITCM (vector table) |
| `build-cr8/zephyr/cr8_sram.bin` | `kakip_cr8_sram.bin` | `0x08180000`, R8SRAM (code and data) |

There is no `zephyr.bin` (`CONFIG_BUILD_OUTPUT_BIN=n`). It would pad the gap
between the two regions to about 135 MB.

If the build fails with `cmsis_core.h: No such file or directory`, run
`west update cmsis`. The Cortex-R8 needs CMSIS 5 in addition to `cmsis_6`.
