# dual_core_hello: CM33 (console + CR8 log relay)

Zephyr on the Kakip's Cortex-M33. It prints `Hello World` and a `tick` every
second on SCI5 (CN6 pin 8/10, 115200 8N1). It also copies whatever the Cortex-R8
writes into the shared log ring to the same UART, tagged `[cr8]`.

How the ring works, and how the two apps fit together, are explained in the
[sample README](../../README.md). Setup, wiring and the U-Boot start line are in
the [board README](../../../../README.md).

## Building

```bash
cd ~/zephyrproject
.venv/bin/west build -p always -b rzv2h_evk/r9a09g057h44gbg/cm33 -d build-cm33 \
    path/to/this/repo/boards/kakip/samples/dual_core_hello/apps/cm33
```

The Kakip overlay (`../../common/kakip_cm33.overlay`) is pulled in automatically
by `CMakeLists.txt`. It moves the console from the EVK's SCI0 to SCI5, which is
what the Kakip brings out on CN6.

Output: `build-cm33/zephyr/zephyr.bin`. Copy it to the SD card as
`kakip_cm33.bin`.

The M33 runs fine on its own. Without the R8 it just keeps printing its own
ticks.
