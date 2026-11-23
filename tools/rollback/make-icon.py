"""Draws the Mawarunco icon and writes every icon file the program uses.

    python tools/rollback/make-icon.py

The icon is the website's favicon: a dark rounded square, a gold ring and a red triangle, defined on a
32 x 32 grid (same numbers as the SVG favicon on the gh-pages branch). Plain Python, no libraries: each
pixel is supersampled against the shapes' distance functions.

Writes (replacing the PCSX2 icons):
  pcsx2/windows/AppIcon.ico              exe icon (resource), installer icon
  bin/resources/icons/AppIconLarge.ico   shortcuts created by the program
  bin/resources/icons/AppIconLarge.png   fullscreen UI, 512 x 512
  pcsx2-qt/resources/icons/AppIcon64.png window icon (Qt resource)
"""

import math
import os
import struct
import zlib

BG = (0x1A, 0x0C, 0x14)
GOLD = (0xC8, 0xA2, 0x4A)
RED = (0xC8, 0x10, 0x2E)

STROKE = 2.0
RING_CENTER = (16.0, 16.0)
RING_RADIUS = 10.0
TRIANGLE = ((16.0, 7.0), (24.0, 21.0), (8.0, 21.0))
CORNER = 6.0


def rounded_rect_inside(x, y):
    # Square 0..32 with corner radius CORNER.
    qx = abs(x - 16.0) - (16.0 - CORNER)
    qy = abs(y - 16.0) - (16.0 - CORNER)
    outside = math.hypot(max(qx, 0.0), max(qy, 0.0))
    inside = min(max(qx, qy), 0.0)
    return outside + inside - CORNER <= 0.0


def ring_hit(x, y):
    return abs(math.hypot(x - RING_CENTER[0], y - RING_CENTER[1]) - RING_RADIUS) <= STROKE / 2


def segment_distance(px, py, a, b):
    ax, ay = a
    bx, by = b
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def triangle_hit(x, y):
    a, b, c = TRIANGLE
    d = min(segment_distance(x, y, a, b), segment_distance(x, y, b, c), segment_distance(x, y, c, a))
    return d <= STROKE / 2


def render(size):
    """Returns RGBA rows (top to bottom) of the icon at size x size pixels."""
    samples = 8 if size <= 64 else 4
    scale = 32.0 / size
    offsets = [(i + 0.5) / samples for i in range(samples)]
    total = samples * samples
    rows = []
    for py in range(size):
        row = bytearray()
        for px in range(size):
            bg = gold = red = 0
            for oy in offsets:
                y = (py + oy) * scale
                for ox in offsets:
                    x = (px + ox) * scale
                    if not rounded_rect_inside(x, y):
                        continue
                    # Painter's order: background, ring, triangle on top.
                    if triangle_hit(x, y):
                        red += 1
                    elif ring_hit(x, y):
                        gold += 1
                    else:
                        bg += 1
            covered = bg + gold + red
            if covered == 0:
                row += b"\0\0\0\0"
                continue
            rgb = [(bg * BG[i] + gold * GOLD[i] + red * RED[i]) / covered for i in range(3)]
            row += bytes([round(rgb[0]), round(rgb[1]), round(rgb[2]), round(255 * covered / total)])
        rows.append(bytes(row))
    return rows


def png_bytes(rows, size):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    raw = b"".join(b"\0" + r for r in rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def dib_bytes(rows, size):
    # 32-bit BGRA, bottom-up, followed by an empty 1-bit AND mask (alpha decides transparency).
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    pixels = bytearray()
    for r in reversed(rows):
        for i in range(0, len(r), 4):
            pixels += bytes([r[i + 2], r[i + 1], r[i], r[i + 3]])
    mask_row = ((size + 31) // 32) * 4
    return header + bytes(pixels) + bytes(mask_row * size)


def ico_bytes(images):
    """images: list of (size, rows). 256 is stored as PNG, smaller sizes as bitmaps (widest support)."""
    entries = []
    for size, rows in images:
        data = png_bytes(rows, size) if size >= 256 else dib_bytes(rows, size)
        entries.append((size, data))
    out = struct.pack("<HHH", 0, 1, len(entries))
    offset = 6 + 16 * len(entries)
    blobs = b""
    for size, data in entries:
        dim = 0 if size >= 256 else size
        out += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
        blobs += data
    return out + blobs


def main():
    repo = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    cache = {}

    def image(size):
        if size not in cache:
            cache[size] = render(size)
        return cache[size]

    def write(rel, data):
        path = os.path.join(repo, rel)
        with open(path, "wb") as f:
            f.write(data)
        print(f"{rel}: {len(data)} bytes")

    write("pcsx2/windows/AppIcon.ico", ico_bytes([(s, image(s)) for s in (256, 48, 32, 24, 16)]))
    write("bin/resources/icons/AppIconLarge.ico", ico_bytes([(s, image(s)) for s in (256, 64, 48, 32, 16)]))
    write("bin/resources/icons/AppIconLarge.png", png_bytes(image(512), 512))
    write("pcsx2-qt/resources/icons/AppIcon64.png", png_bytes(image(64), 64))


if __name__ == "__main__":
    main()
