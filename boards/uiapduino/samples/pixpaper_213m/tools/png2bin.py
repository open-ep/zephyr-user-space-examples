#!/usr/bin/env python3
# Convert a PNG into the raw 4000-byte packed frame the firmware's "load"
# command expects. Use with a terminal's binary send-file function
# (e.g. TeraTerm: type "load", then File -> Send file... with Binary checked).
#
# usage: python3 png2bin.py picture.png [out.bin]

import sys
import cv2

IMG_WIDTH = 250
IMG_HEIGHT = 122
GROUPS = 16

if len(sys.argv) < 2:
    sys.exit("usage: png2bin.py <image.png> [out.bin]")
src = sys.argv[1]
dst = sys.argv[2] if len(sys.argv) > 2 else src.rsplit(".", 1)[0] + ".bin"

im = cv2.imread(src, cv2.IMREAD_GRAYSCALE)
if im is None:
    sys.exit(f"cannot read {src}")
im = cv2.resize(im, (IMG_WIDTH, IMG_HEIGHT))

out = bytearray()
for x in range(IMG_WIDTH):
    for g in range(GROUPS):
        b = 0
        for bit in range(8):
            y = g * 8 + bit
            px = 1 if (y >= IMG_HEIGHT or im[y, x] >= 128) else 0
            b |= px << (7 - bit)
        out.append(b)

with open(dst, "wb") as f:
    f.write(out)
print(f"OK: {dst} ({len(out)} bytes)")
