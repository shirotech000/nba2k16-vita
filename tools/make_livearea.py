#!/usr/bin/env python3
"""makes placeholder livearea pngs (plain basketball, no game art).
vita wants 8-bit indexed pngs so we write them by hand, no PIL needed."""
import math
import os
import struct
import sys
import zlib

PALETTE = [
    (16, 18, 28),     # 0 bg
    (232, 116, 32),   # 1 ball
    (40, 22, 12),     # 2 seams
    (250, 160, 80),   # 3 highlight
    (32, 36, 52),     # 4 floor stripe
]


def chunk(tag, data):
    c = struct.pack(">I", len(data)) + tag + data
    return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def write_png(path, w, h, px):
    raw = b"".join(b"\x00" + bytes(px[y * w:(y + 1) * w]) for y in range(h))
    plte = b"".join(bytes(c) for c in PALETTE)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 3, 0, 0, 0))
    png += chunk(b"PLTE", plte)
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(png)


def draw(w, h, cx, cy, r, stripes=False):
    px = []
    for y in range(h):
        for x in range(w):
            c = 4 if stripes and (y // 12) % 2 == 0 else 0
            dx, dy = (x - cx) / r, (y - cy) / r
            d = math.hypot(dx, dy)
            if d <= 1.0:
                c = 1
                if math.hypot(dx + 0.35, dy + 0.35) < 0.3:
                    c = 3
                # seams: vertical, horizontal and two curved ones
                lw = 0.045
                if abs(dx) < lw or abs(dy) < lw:
                    c = 2
                if abs(math.hypot(dx - 1.25, dy) - 0.95) < lw or abs(math.hypot(dx + 1.25, dy) - 0.95) < lw:
                    c = 2
                if d > 0.96:
                    c = 2
            px.append(c)
    return px


def main(root):
    sce = os.path.join(root, "sce_sys")
    write_png(os.path.join(sce, "icon0.png"), 128, 128, draw(128, 128, 64, 64, 58))
    write_png(os.path.join(sce, "livearea/contents/bg.png"), 840, 500, draw(840, 500, 420, 250, 200, True))
    write_png(os.path.join(sce, "livearea/contents/startup.png"), 280, 158, draw(280, 158, 140, 79, 70))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), ".."))
