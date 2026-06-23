# pixpaper-213m e-paper (raw SPI)

Drive an **Open-EP pixpaper-213m** 2.13" monochrome e-paper panel (250×122) on
the FRDM-IMX93 Cortex-M33, directly over raw LPSPI3 — no display subsystem: the
panel command sequence is issued by hand and DC/RST/BUSY are plain GPIOs.

> Platform prerequisites — the kernel clock patch, the LPSPI3 SoC node, and the
> remoteproc start/stop flow — are in the [board README](../../README.md). Do
> those first.

## Apps

| App | What it does |
| --- | ------------ |
| [image](apps/image/) | Show a bundled image (`png_HEX.h`). |

All apps share the panel binding (`binding/`), the vendor-prefix patch
(`patches/`), and the wiring overlay (`common/`).

## Wiring

| Signal | Pin                   | Notes                                       |
| ------ | --------------------- | ------------------------------------------- |
| SPI    | LPSPI3, hardware PCS0 | SCK=IO11 / SIN=IO09 / SOUT=IO10 / PCS0=IO08 |
| DC     | GPIO2_IO00            | low = command, high = data                  |
| RST    | GPIO2_IO05            | low = reset                                 |
| BUSY   | GPIO2_IO26            | high = busy                                 |

SPI runs at 5 MHz in mode 0, defined by `common/frdm_imx93_mimx9352_m33.overlay`
(shared by every app under `apps/`).

## Install into a Zephyr tree

```bash
ZEPHYR=/path/to/zephyrproject/zephyr
HERE=/path/to/this-repo/boards/frdm_imx93/samples/pixpaper_213m

# 1. Panel apps + shared wiring (keep apps/ and common/ side by side)
DST="$ZEPHYR/samples/boards/nxp/frdm_imx93/pixpaper_213m"
mkdir -p "$DST"
cp -r "$HERE"/apps "$HERE"/common "$DST/"

# 2. Panel devicetree binding
cp "$HERE"/binding/open-ep,pixpaper-213m.yaml "$ZEPHYR/dts/bindings/display/"

# 3. Register the vendor prefix
cd "$ZEPHYR"
git apply "$HERE"/patches/0001-dts-bindings-add-open-ep-vendor-prefix.patch
```

(The LPSPI3 SoC node is a board-level patch — see the board README.)

Then build an app — see its README, e.g. [apps/image](apps/image/).
