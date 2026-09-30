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
| [Kakip (RZ/V2H)](boards/kakip/) | Cortex-M33 + Cortex-R8, started from U-Boot | none (UART log demo) | [dual_core_hello](boards/kakip/samples/dual_core_hello/) |
| [UIAPduino Pro Micro CH32V003](boards/uiapduino/) | CH32V003 (RISC-V, standalone) | Open-EP pixpaper-213m (2.13", bit-banged SPI) | [pixpaper_213m](boards/uiapduino/samples/pixpaper_213m/) |
| [W6300-EVB-Pico2](boards/w6300_evb_pico2/) | RP2350 (Cortex-M33, standalone) + W6300 Ethernet | Open-EP pixpaper-213m (2.13" mono) / pixpaper-213c (2.13" 4-colour) | [pixpaper_213m](boards/w6300_evb_pico2/samples/pixpaper_213m/), [pixpaper_213c](boards/w6300_evb_pico2/samples/pixpaper_213c/) |

## How it works

On remoteproc platforms, the companion core (e.g. the i.MX93 Cortex-M33) runs
a Zephyr firmware that drives the panel over SPI + GPIO. Linux, on the
application cores, loads and starts that firmware via remoteproc (`zephyr.elf`
in `/lib/firmware`, then `echo start > /sys/class/remoteproc/.../state`).
Because the M-core does not bring up its own peripheral clocks, the platform
usually needs a small kernel change so Linux keeps those clocks enabled — see
each board's README.

On standalone MCU boards (e.g. the UIAPduino CH32V003 or the RP2350-based
W6300-EVB-Pico2), the Zephyr firmware is the whole system: it draws at boot and
is flashed over the board's USB bootloader — no Linux involved. How much the
firmware can do scales with the board: 16 KB of flash fits a mono image and a
game, while 2 MB fits 4-colour images, 4-level grayscale and a video clip.

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
