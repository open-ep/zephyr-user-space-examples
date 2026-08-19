#!/usr/bin/env python3
"""PIXPAPER-213-M 4-gray converter: PNG -> two 1bpp planes (0x24 MSB, 0x26 LSB).

Gray coding per pixel (2 bits): 11=white, 10=light gray, 01=dark gray,
00=black. Packing matches the mono panel stream order: 250 columns x 16
bytes, MSB-first within a byte, written with the same cursor/window flow as
the normal mono image.
"""
import sys
import cv2
import numpy as np

W, H, STRIDE = 250, 122, 16

args = [a for a in sys.argv[1:] if not a.startswith("--")]
src = args[0]
out = args[1] if len(args) > 1 else "img_gray4.h"
prefix = args[2] if len(args) > 2 else "img_gray4"
ROTCCW = "--rotccw" in sys.argv
FIT = "--fit" in sys.argv

im = cv2.imread(src, cv2.IMREAD_GRAYSCALE)
if im is None:
    raise SystemExit(f"cannot read {src}")
if ROTCCW:
    im = cv2.rotate(im, cv2.ROTATE_90_COUNTERCLOCKWISE)
if FIT:
    # preserve aspect ratio, pad with white
    s = min(W / im.shape[1], H / im.shape[0])
    nw, nh = max(1, int(im.shape[1] * s)), max(1, int(im.shape[0] * s))
    scaled = cv2.resize(im, (nw, nh))
    canvas = np.full((H, W), 255, dtype=np.uint8)
    x0, y0 = (W - nw) // 2, (H - nh) // 2
    canvas[y0:y0 + nh, x0:x0 + nw] = scaled
    im = canvas
else:
    im = cv2.resize(im, (W, H))
im = cv2.normalize(im, None, 0, 255, cv2.NORM_MINMAX)

# 4-level quantize: 0..63->0(black) 64..127->1(dark) 128..191->2(light) ->3
lv = np.clip(im // 64, 0, 3).astype(np.uint8)

# reference mapping (pixpaper-213-m-test-frdm-imx93.c): level 0..3 ->
# bit24 = {1,0,1,0}[lv], bit26 = {1,1,0,0}[lv]  (black=1,1 ... white=0,0)
msb = 1 - (lv & 1)      # 0x24 plane
lsb = 1 - (lv >> 1)     # 0x26 plane


def pack(plane):
    # same layout the mono demos stream: for x in 0..249: 16 bytes, bit k of
    # byte b = pixel y = b*8+k; y >= 122 pads to 1
    data = []
    for x in range(W):
        for b in range(STRIDE):
            v = 0
            for k in range(8):
                y = b * 8 + k
                bit = 0 if y >= H else int(plane[y, x])   # pad = level 3 (white) bits
                v |= bit << (7 - k)
            data.append(v)
    return data


p24, p26 = pack(msb), pack(lsb)

with open(out, "w") as f:
    f.write(f"#ifndef {prefix.upper()}_H\n#define {prefix.upper()}_H\n\n#include <stdint.h>\n\n")
    f.write(f"// 4-gray planes for: {src} (ref mapping: black=11 dark=01 light=10 white=00)\n")
    for name, data in ((f"{prefix}_p24", p24), (f"{prefix}_p26", p26)):
        f.write(f"static const uint8_t {name}[{len(data)}] = {{\n")
        for i in range(0, len(data), 12):
            f.write("    " + ", ".join(f"0x{v:02X}" for v in data[i:i+12]) + ",\n")
        f.write("};\n\n")
    f.write("#endif\n")
print(f"wrote {out} ({len(p24)}+{len(p26)} bytes)")

# preview
prev = (lv * 85).astype(np.uint8)
cv2.imwrite(out.replace(".h", "_preview.png"), cv2.resize(prev, (W*2, H*2), interpolation=cv2.INTER_NEAREST))
