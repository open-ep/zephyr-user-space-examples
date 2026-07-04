# Zephyr e-paper examples

Zephyr firmware that drives e-paper panels — from a companion core started by
the host Linux via remoteproc, or standalone on a small MCU — plus the kernel,
devicetree, and board-support patches each platform needs. Examples are
organized per board under `boards/`, so more boards and SoCs can be added
alongside the existing ones.

## Supported

| Board | Core | Panel | Sample |
| ----- | ---- | ----- | ------ |
| [FRDM-IMX93](boards/frdm_imx93/) | Cortex-M33 | Open-EP pixpaper-213m (2.13", raw SPI) | [pixpaper_213m](boards/frdm_imx93/samples/pixpaper_213m/) |
| [UIAPduino Pro Micro CH32V003](boards/uiapduino/) | CH32V003 (RISC-V, standalone) | Open-EP pixpaper-213m (2.13", bit-banged SPI) | [pixpaper_213m](boards/uiapduino/samples/pixpaper_213m/) |

## How it works

On remoteproc platforms, the companion core (e.g. the i.MX93 Cortex-M33) runs
a Zephyr firmware that drives the panel over SPI + GPIO. Linux, on the
application cores, loads and starts that firmware via remoteproc (`zephyr.elf`
in `/lib/firmware`, then `echo start > /sys/class/remoteproc/.../state`).
Because the M-core does not bring up its own peripheral clocks, the platform
usually needs a small kernel change so Linux keeps those clocks enabled — see
each board's README.

On standalone MCU boards (e.g. the UIAPduino CH32V003), the Zephyr firmware is
the whole system: it draws at boot and is flashed over the board's USB
bootloader — no Linux involved.

## Layout

```
boards/<board>/
  README.md            Platform notes + kernel/SoC patches (per board/SoC)
  patches/kernel/      Linux kernel patches for this board
  patches/zephyr/      Zephyr SoC/devicetree patches for this board
  samples/<panel>/
    README.md          Wiring, build, panel sequence (per panel)
    patches/           Panel-specific Zephyr patches (e.g. vendor prefix)
    binding/           Devicetree binding for the panel
    ...                Zephyr application (CMakeLists.txt, prj.conf, src, ...)
```

Start with the board README for your platform, then the sample README for your
panel.
