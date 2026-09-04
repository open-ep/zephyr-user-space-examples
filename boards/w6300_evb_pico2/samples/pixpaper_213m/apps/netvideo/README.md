# netvideo — push frames to the panel over Ethernet

Plug in the network cable and USB-C, and the board shows its DHCP address on
the panel. Point the sender at that address and it streams frames from any
video, GIF, webcam or screen region to the e-paper — or uploads a whole short
clip into RAM so the board loops it with the network unplugged.

```
PC (epdstream.py) --Ethernet TCP:5001--> W6300-EVB-Pico2 --bit-banged SPI--> PIXPAPER-213-M
```

The PC does everything expensive — decode, scale, gamma, threshold, and packing
into the panel's RAM byte order — so the board only pushes ready-made 4000-byte
frames at the controller. No filesystem, no flash writes: frames live in RAM
and are gone at power-off.

## Before you build: two driver fixes

The upstream `eth_w6300` driver has two bugs that this app will hit within one
upload — the chip's TX path wedges and the board disappears from the network
until it is power-cycled. Fixes are submitted upstream as
[zephyrproject-rtos/zephyr#117112](https://github.com/zephyrproject-rtos/zephyr/pull/117112);
until they land, apply them to your Zephyr tree:

```bash
cd ~/zephyrproject/zephyr
git apply /path/to/this-repo/boards/w6300_evb_pico2/patches/zephyr/0001-drivers-ethernet-w6300-wait-for-TX-free-space.patch
git apply /path/to/this-repo/boards/w6300_evb_pico2/patches/zephyr/0002-drivers-ethernet-w6300-service-interrupts-from-monitor.patch
```

The copies in this repo are pinned to the version tested against these
samples, and work offline. To pull whatever the pull request currently holds
instead — useful if review changed it — fetch it directly:

```bash
curl -L https://github.com/zephyrproject-rtos/zephyr/pull/117112.patch | git apply
```

As of August 2026 the pull request is open and awaiting first review, so expect
to need these for a while. The two changes total about thirty lines and are
independent of each other: the TX one is what stops the lock-up, the interrupt
one hardens a second path that has no recovery of its own.

Measured with and without, same board and same command: unpatched died on the
**first** upload (frame 10 of 96 at the board's default 500 kHz, frame 14 at
8 MHz) and needed a power cycle each time; patched completed **10 out of 10**
uploads. If your `git apply` fails because the file moved, the two changes are
small enough to redo by hand — see the patch headers for the reasoning.

## Build and flash

```bash
cd ~/zephyrproject
west build -p always -b w6300_evb_pico2/rp2350a/m33 \
    path/to/this-repo/boards/w6300_evb_pico2/samples/pixpaper_213m/apps/netvideo
```

Hold BOOTSEL while plugging in USB, then copy `build/zephyr/zephyr.uf2` onto
the `RP2350` drive. The panel will show:

```
EPD STREAM READY
 192.168.1.121
   PORT 5001
```

That screen is drawn with a built-in 5x7 font — no console needed, which is the
point: the board tells you its own address.

If there is no DHCP server (a direct cable to your PC), it falls back to
`192.168.7.2/24` after 15 seconds; give your PC an address in the same subnet.

## Sending

The sender needs Python with `opencv-python` and `numpy`:

```bash
python3 ../../tools/epdstream.py <panel-ip> clip.mp4 --fps 5 --upload
```

On Windows use `py` or `python` — `python3` there is usually a Microsoft Store
alias that silently does nothing.

| Mode | Command | What happens |
| --- | --- | --- |
| **Clip** (recommended) | `... clip.mp4 --fps 5 --upload` | Up to 96 frames go into board RAM, then it loops them by itself. Disconnect freely. |
| **Live** | `... clip.mp4 --fps 2` | Frames stream continuously; the panel always shows the newest one. Keep `--fps` low. |
| Screen | `... screen --region 0,0,1280,720` | Live mirror of a screen region (needs `ffmpeg`). |
| Webcam | `... 0` | Live from camera index 0. |

Useful flags: `--gamma 1.6` brightens midtones (e-paper is contrasty),
`--dither bayer` for photos, `--dither none` (default) for text and line art,
`--fit crop|pad|stretch` for aspect handling.

## Why clip mode exists

The W6300 on this board sits on a bit-banged SPI bus, so the practical ceiling
is roughly 20 KB/s — about 5 frames per second, with nothing to spare. Live
streaming above 2 fps starves and stalls. Clip mode sidesteps the link
entirely: upload once at whatever speed the link manages, then play back from
RAM at full speed.

The cost is length. 96 frames × 4000 B = 384 KB of the RP2350's 520 KB, which
is why the build reports ~93 % RAM. At 5 fps that is a 19-second loop; longer
sources are truncated by the sender, which tells you when it does.

## Protocol

Little endian, TCP port 5001. Hello: `"EPDS"` + `u8 version(1)` + `u8 mode(0)`
+ `u16 reserved`. Then `u32` commands, repeatedly:

| Command | Payload | Meaning |
| --- | --- | --- |
| `4000` | 4000-byte frame | Live frame, display as soon as the panel is free |
| `1` | — | Full refresh (repaint current frame, clears ghosting) |
| `2` | — | Clear to white |
| `3` | `u32 count`, `u32 frame_us`, then `count` frames | Clip upload; the board acks **every** frame with one byte, then loops |

Frame packing: `frame[x * 16 + (y >> 3)]`, bit `0x80 >> (y & 7)`, `1` = white.
Identical to the `.epdv` payload used by the offline [video](../video/) app, so
`tools/video2epd.py` output and this sender agree byte for byte.

The per-frame ack in clip mode is application-level flow control. Without it a
384 KB burst overruns the network buffers and the connection is reset — which
is also why `CONFIG_NET_TCP_MAX_RECV_WINDOW_SIZE` is set to 2048, comfortably
under the 8 KB RX buffer pool. Never advertise a window your buffers cannot
absorb.

## Devicetree

Panel wiring comes from the shared
[`../../common/w6300_evb_pico2.overlay`](../../common/w6300_evb_pico2.overlay);
this app adds [`ethernet.overlay`](ethernet.overlay) for two things:

- **`spi-max-frequency = <8000000>`** — the board devicetree caps the W6300's
  bit-banged bus at 500 kHz, well below what this app wants. Raising the cap
  lets the bit-bang driver run as fast as the CPU can toggle pins.
- **a fixed MAC** — with `zephyr,random-mac-address` the DHCP server sees a new
  client on every boot and hands out a new IP every time. A fixed
  locally-administered address (`02:...`) keeps the panel's IP stable, and lets
  you reserve it on your router. Change the last bytes if you run more than one
  board on the same LAN.

## SBOM and VEX

Zephyr can generate a build-accurate SPDX SBOM — it hooks the CMake file-based
API, so it lists the files that actually went into `zephyr.elf`, not the whole
tree. Order matters: `--init` must run against a build directory that
`west build` will NOT wipe afterwards (`-p always` deletes the query file).

```bash
west spdx --init -d build
west build -b w6300_evb_pico2/rp2350a/m33 -d build path/to/netvideo \
    -- -DCONFIG_BUILD_OUTPUT_META=y
west spdx -d build --analyze-includes --include-sdk
# -> build/spdx/{app,zephyr,build,sdk,modules-deps}.spdx
```

Two things worth knowing before you feed those files to a scanner:

- **Vulnerability scanners can't map them.** The packages carry no PURL/CPE,
  so osv-scanner and friends report zero packages. For Zephyr the practical
  CVE feed is the [official vulnerability list]
  (https://docs.zephyrproject.org/latest/security/vulnerabilities.html),
  matched against your tree by hand.
- **That is what the VEX is for.** [`sbom/netvideo.openvex.json`](sbom/netvideo.openvex.json)
  is our OpenVEX statement covering the Zephyr CVEs published at the time of
  writing: all `not_affected`, each with the concrete reason from this app's
  `.config` (no USB, no Bluetooth, no Wi-Fi, no USERSPACE/SMP, no disk — the
  vulnerable components are simply not compiled in). The only attack surface
  this firmware has is IPv4/TCP/DHCPv4, so networking CVEs are the ones to
  re-check when the list grows.

## Panel discipline

Full-frame writer: the whole frame goes to `0x24` on every kick, `0x37` byte 5
is `0x40` (ping-pong ON) so the controller diffs the banks itself, and `0x26` is
never touched. Same as the [video](../video/) app, and the opposite of what the
[showcase](../showcase/) games do — see the
[sample README](../../README.md#the-two-partial-update-disciplines) before
mixing the two.
