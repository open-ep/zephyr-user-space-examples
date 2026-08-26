#!/usr/bin/env python3
"""Stream frames to the pixpaper netvideo board over TCP.

The PC does everything (decode, scale, threshold, pack into controller RAM
order); the board just displays. Sources: a video file, a webcam index, or
the live screen (X11, needs ffmpeg).

examples:
  python3 epdstream.py 192.168.1.50 clip.mp4
  python3 epdstream.py 192.168.1.50 clip.mp4 --fps 5 --gamma 1.6
  python3 epdstream.py 192.168.1.50 screen --region 0,0,1280,720
  python3 epdstream.py 192.168.1.50 0                # webcam 0
"""
import argparse
import socket
import struct
import subprocess
import sys
import time

import cv2
import numpy as np

W, H, STRIDE = 250, 122, 16
FRAME_BYTES = W * STRIDE
CMD_REFRESH = 1
CMD_CLIP = 3
CLIP_MAX_FRAMES = 96

BAYER8 = (np.array([
    [0, 32,  8, 40,  2, 34, 10, 42],
    [48, 16, 56, 24, 50, 18, 58, 26],
    [12, 44,  4, 36, 14, 46,  6, 38],
    [60, 28, 52, 20, 62, 30, 54, 22],
    [3, 35, 11, 43,  1, 33,  9, 41],
    [51, 19, 59, 27, 49, 17, 57, 25],
    [15, 47,  7, 39, 13, 45,  5, 37],
    [63, 31, 55, 23, 61, 29, 53, 21]], dtype=np.float32) + 0.5) / 64.0


def to_panel(gray, fit, gamma, dither, thresh):
    h, w = gray.shape
    if fit == "crop":
        s = max(W / w, H / h)
        rw, rh = int(w * s + 0.5), int(h * s + 0.5)
        g = cv2.resize(gray, (rw, rh))
        x0, y0 = (rw - W) // 2, (rh - H) // 2
        g = g[y0:y0 + H, x0:x0 + W]
    elif fit == "pad":
        s = min(W / w, H / h)
        rw, rh = max(1, int(w * s)), max(1, int(h * s))
        g = np.full((H, W), 255, np.uint8)
        r = cv2.resize(gray, (rw, rh))
        g[(H - rh) // 2:(H - rh) // 2 + rh, (W - rw) // 2:(W - rw) // 2 + rw] = r
    else:
        g = cv2.resize(gray, (W, H))
    if gamma != 1.0:
        g = (np.power(g / 255.0, 1.0 / gamma) * 255).astype(np.uint8)
    if dither == "bayer":
        t = np.tile(BAYER8, (H // 8 + 1, W // 8 + 1))[:H, :W]
        bits = (g / 255.0 > t)
    else:
        bits = g >= thresh
    return bits                             # bool, True = white


def pack(bits):
    """panel RAM order: byte = frame[x*16 + (y>>3)], bit 0x80>>(y&7)."""
    col = np.zeros((W, STRIDE * 8), dtype=np.uint8)
    col[:, :H] = bits.T                     # x-major, pad rows 122..127 = 0
    col[:, H:] = 1                          # padding = white
    return np.packbits(col, axis=1).tobytes()


def frames_from(source, region, src_fps):
    if source == "screen":
        x, y, w, h = (int(v) for v in region.split(","))
        cmd = ["ffmpeg", "-v", "error", "-f", "x11grab", "-framerate",
               str(src_fps), "-video_size", f"{w}x{h}", "-i", f":0.0+{x},{y}",
               "-f", "rawvideo", "-pix_fmt", "gray", "-"]
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
        n = w * h
        while True:
            raw = proc.stdout.read(n)
            if len(raw) < n:
                return
            yield np.frombuffer(raw, np.uint8).reshape(h, w)
    else:
        cap = cv2.VideoCapture(int(source) if source.isdigit() else source)
        if not cap.isOpened():
            sys.exit(f"cannot open {source}")
        while True:
            ok, frame = cap.read()
            if not ok:
                if not source.isdigit():    # loop video files
                    cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
                    continue
                return
            yield cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host")
    ap.add_argument("source", help="video file, webcam index, or 'screen'")
    ap.add_argument("--port", type=int, default=5001)
    ap.add_argument("--fps", type=float, default=5.0)
    ap.add_argument("--fit", choices=["crop", "pad", "stretch"], default="crop")
    ap.add_argument("--gamma", type=float, default=1.0)
    ap.add_argument("--dither", choices=["none", "bayer"], default="none")
    ap.add_argument("--thresh", type=int, default=128)
    ap.add_argument("--region", default="0,0,1920,1080",
                    help="screen source: x,y,w,h")
    ap.add_argument("--full-every", type=int, default=300,
                    help="frames between full refreshes (ghosting payoff)")
    ap.add_argument("--upload", action="store_true",
                    help="upload the whole clip to board RAM, then it loops "
                         f"on its own at --fps (max {CLIP_MAX_FRAMES} frames)")
    a = ap.parse_args()

    def connect(fatal=True):
        try:
            c = socket.create_connection((a.host, a.port), timeout=10)
        except (TimeoutError, OSError):
            if fatal:
                sys.exit(f"cannot reach {a.host}:{a.port} - check the IP "
                         "shown on the panel, and give the board ~20 s after "
                         "power-on for DHCP")
            raise
        c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        c.sendall(b"EPDS" + bytes([1, 0, 0, 0]))
        return c

    if a.upload:
        if a.source == "screen" or a.source.isdigit():
            sys.exit("--upload needs a video file source")
        cap = cv2.VideoCapture(a.source)
        src_fps = cap.get(cv2.CAP_PROP_FPS) or 30
        step = max(1, round(src_fps / a.fps))
        frames, i = [], 0
        while len(frames) < CLIP_MAX_FRAMES:
            ok, frm = cap.read()
            if not ok:
                break
            if i % step == 0:
                gray = cv2.cvtColor(frm, cv2.COLOR_BGR2GRAY)
                bits = to_panel(gray, a.fit, a.gamma, a.dither, a.thresh)
                frames.append(pack(bits))
            i += 1
        if not frames:
            sys.exit("no frames decoded")
        total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT) or 0)
        if total and total / src_fps * a.fps > CLIP_MAX_FRAMES:
            print(f"note: clip truncated to {len(frames)} frames "
                  f"({len(frames)/a.fps:.0f}s at {a.fps} fps)")
        frame_us = int(1_000_000 / a.fps)
        for attempt in range(1, 4):
            try:
                s = connect(fatal=(attempt == 1))
                s.settimeout(15)        # per-frame ack must arrive within 15 s
                print(f"connected to {a.host}:{a.port}, uploading "
                      f"{len(frames)} frames")
                s.sendall(struct.pack("<III", CMD_CLIP, len(frames), frame_us))
                t0 = time.time()
                for n, f in enumerate(frames, 1):
                    s.sendall(f)
                    if s.recv(1) == b"":    # board acks every frame
                        raise ConnectionResetError
                    print(f"\r  uploading {n}/{len(frames)} "
                          f"({n*4000/1024/(time.time()-t0+1e-9):.0f} KB/s)",
                          end="", flush=True)
                print(f"\ndone in {time.time()-t0:.0f}s - the board now loops "
                      "the clip on its own; you can disconnect")
                s.close()
                return
            except OSError as e:        # reset, timeout, or unreachable
                print(f"\n  upload failed (attempt {attempt}/3): "
                      f"{type(e).__name__}: {e} - reconnecting in 3 s...")
                time.sleep(3)
        sys.exit("upload failed 3 times - power-cycle the board and retry "
                 "(and make sure it runs the per-frame-ack firmware)")

    s = connect()
    s.settimeout(None)                  # sends block instead of timing out
    print(f"connected to {a.host}:{a.port}, streaming at {a.fps} fps")

    period = 1.0 / a.fps
    sent = 0
    t0 = time.time()
    for gray in frames_from(a.source, a.region, max(a.fps, 1)):
        t_next = t0 + sent * period
        now = time.time()
        if now < t_next:
            time.sleep(t_next - now)
        bits = to_panel(gray, a.fit, a.gamma, a.dither, a.thresh)
        t_send = time.time()
        s.sendall(struct.pack("<I", FRAME_BYTES) + pack(bits))
        stall = time.time() - t_send
        if stall > 2.0:
            print(f"  link is slow: frame {sent} took {stall:.1f}s to send "
                  "(board SPI backlog) - consider a lower --fps")
        sent += 1
        if a.full_every and sent % a.full_every == 0:
            s.sendall(struct.pack("<I", CMD_REFRESH))
        if sent % 50 == 0:
            fps = sent / (time.time() - t0)
            print(f"  {sent} frames, effective {fps:.1f} fps")


if __name__ == "__main__":
    try:
        main()
    except (KeyboardInterrupt, BrokenPipeError):
        print("\nstopped")
