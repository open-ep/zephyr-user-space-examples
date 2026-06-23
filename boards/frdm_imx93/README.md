# FRDM-IMX93 — Cortex-M33 e-paper (Zephyr + remoteproc)

Platform notes shared by all e-paper samples on the **FRDM-IMX93 Cortex-M33**.
The M33 firmware is built with Zephyr and started from Linux (on the A55) via
remoteproc.

Samples on this board:

- [pixpaper_213m](samples/pixpaper_213m/) — Open-EP pixpaper-213m, raw SPI.

## Prerequisites

- A Linux image with the `imx-rproc` remoteproc driver enabled.
- The kernel and Zephyr patches below applied.
- Linux must not drive the hardware the M33 owns (see *Resource ownership*).

## Kernel patch — keep the M33's clocks enabled

The M33 does not enable its own peripheral root/gate clocks; it assumes they are
already running. Linux turns off "unused" clocks late in boot
(`clk_disable_unused`), so any peripheral the M33 touches must be kept enabled on
the Linux side, or the SoC stalls (AHB/AXI bus stall) on first access. The
`clk_ignore_unused` boot arg does not help, because these clocks are already off
by then.

The patch marks the clocks the M33 uses as `CLK_IS_CRITICAL` in
`drivers/clk/imx/clk-imx93.c` (the same approach the SoC already uses for
`cm33` / `m33_root`):

| Clock            | Purpose                  |
| ---------------- | ------------------------ |
| `lpuart2_root`   | M33 console root         |
| `lpuart2` (gate) | M33 console gate         |
| `lpspi3_root`    | e-paper SPI root         |
| `lpspi3` (gate)  | e-paper SPI gate         |
| `gpio2` (gate)   | DC/RST/BUSY control pins |

Apply (generated against the NXP i.MX BSP kernel `linux-imx` 6.12.34). The
change is tiny — just a flag on five clock-table entries — so it should also
apply to nearby versions; if it doesn't, make the five edits from the table
above by hand.

```bash
cd /path/to/linux-imx
git apply /path/to/this-repo/boards/frdm_imx93/patches/kernel/0001-clk-imx93-keep-m33-peripheral-clocks-critical.patch
```

After booting the patched kernel, `enable_count` for these clocks should be ≥ 1:

```console
grep -E 'lpuart2|lpspi3|gpio2' /sys/kernel/debug/clk/clk_summary
```

## Zephyr patch — add the LPSPI3 node

The i.MX93 M33 SoC dtsi has no LPSPI3 node, so panel overlays cannot enable
`&lpspi3`. This patch adds it.

The patch was generated against upstream Zephyr commit `6c7aadb0fc6`
(`v4.4.0-2084-g6c7aadb`). Fetch Zephyr at that revision via the official west
workflow (install `west` and the Zephyr SDK first — see the
[Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/))
and apply on top:

```bash
# Get the Zephyr source at the matching revision
west init -m https://github.com/zephyrproject-rtos/zephyr --mr 6c7aadb0fc6 zephyrproject
cd zephyrproject
west update
cd zephyr

# Apply the patch
git apply /path/to/this-repo/boards/frdm_imx93/patches/zephyr/0001-soc-nxp-imx93-m33-add-lpspi3-node.patch
```

The patch only adds a devicetree node, so it should also apply to a recent
Zephyr `main` or a nearby `v4.4.x` release; if it doesn't, add the `lpspi3`
node to `dts/arm/nxp/imx/nxp_imx93_m33.dtsi` by hand.

## Running via remoteproc

Copy the built firmware to the Linux firmware search path and start the M33 from
the A55:

```console
# On the host, after building a sample:
scp build/zephyr/zephyr.elf root@<board-ip>:/lib/firmware/

# On the board:
cat /sys/class/remoteproc/remoteproc0/name      # expect: imx-rproc
echo zephyr.elf > /sys/class/remoteproc/remoteproc0/firmware
echo start      > /sys/class/remoteproc/remoteproc0/state
```

To stop or reload:

```console
echo stop  > /sys/class/remoteproc/remoteproc0/state
# after copying a new zephyr.elf, repeat the firmware + start steps
echo start > /sys/class/remoteproc/remoteproc0/state
```

The M33 log appears on LPUART2 (115200 8N1).

## Resource ownership

Do not let Linux drive the hardware the M33 owns. In particular, do **not** apply
a device tree that binds `spidev` to LPSPI3 or claims the DC/RST/BUSY GPIOs —
Linux and the M33 would fight over the controller. Keeping the clocks
`CLK_IS_CRITICAL` only keeps them powered; it does not bind any Linux driver,
which is the intended state.
