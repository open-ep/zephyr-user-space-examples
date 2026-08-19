#!/usr/bin/env python3
"""Embed an .epdv clip (from video2epd.py) as a C header for the Zephyr
player: the RP2350 has no filesystem, so the whole clip lives in flash.

usage: python3 epdv2h.py clip.epdv src/clip_epdv.h
"""
import struct
import sys

src = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else "clip_epdv.h"

data = open(src, "rb").read()
magic, ver, flags, w, h, frame_us, count, fbytes = \
    struct.unpack_from("<4sHHHHIII", data, 0)
assert magic == b"EPDV", "not an .epdv file"
frames = data[64:]
assert len(frames) == count * fbytes, "truncated clip"

with open(out, "w") as f:
    f.write("#ifndef CLIP_EPDV_H\n#define CLIP_EPDV_H\n\n#include <stdint.h>\n\n")
    f.write(f"/* {src}: {w}x{h}, {count} frames, {frame_us} us/frame */\n")
    f.write(f"#define CLIP_FRAME_COUNT {count}\n")
    f.write(f"#define CLIP_FRAME_BYTES {fbytes}\n")
    f.write(f"#define CLIP_FRAME_US    {frame_us}\n\n")
    f.write(f"static const uint8_t clip_frames[{len(frames)}] = {{\n")
    for i in range(0, len(frames), 12):
        f.write("    " + ", ".join(f"0x{b:02X}" for b in frames[i:i+12]) + ",\n")
    f.write("};\n\n#endif\n")
print(f"wrote {out}: {count} frames x {fbytes} B = {len(frames)} B, "
      f"{frame_us} us/frame")
