#!/usr/bin/env python3
"""Generates resources/livewall.ico (no third-party dependencies).

The icon is a rounded square with a diagonal gradient and a white play glyph,
rendered with 3x supersampling for smooth edges.
"""

import math
import os
import struct
import zlib

SIZES = [16, 24, 32, 48, 64, 128, 256]
SS = 3  # supersampling factor per axis

TOP_LEFT = (99, 102, 241)
BOTTOM_RIGHT = (14, 165, 233)
TRIANGLE = ((0.37, 0.27), (0.73, 0.50), (0.37, 0.73))


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def point_in_triangle(px, py, tri):
    (x1, y1), (x2, y2), (x3, y3) = tri
    d1 = (px - x2) * (y1 - y2) - (x1 - x2) * (py - y2)
    d2 = (px - x3) * (y2 - y3) - (x2 - x3) * (py - y3)
    d3 = (px - x1) * (y3 - y1) - (x3 - x1) * (py - y1)
    has_neg = d1 < 0 or d2 < 0 or d3 < 0
    has_pos = d1 > 0 or d2 > 0 or d3 > 0
    return not (has_neg and has_pos)


def inside_rounded_rect(x, y, size, radius):
    r = radius
    cx = min(max(x, r), size - r)
    cy = min(max(y, r), size - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def render(size):
    radius = size * 0.22
    pixels = bytearray(size * size * 4)

    for py in range(size):
        for px in range(size):
            r_acc = g_acc = b_acc = a_acc = 0
            for sy in range(SS):
                for sx in range(SS):
                    x = px + (sx + 0.5) / SS
                    y = py + (sy + 0.5) / SS
                    if not inside_rounded_rect(x, y, size, radius):
                        continue
                    t = (x / size + y / size) * 0.5
                    color = lerp(TOP_LEFT, BOTTOM_RIGHT, t)
                    if point_in_triangle(x / size, y / size, TRIANGLE):
                        color = (255, 255, 255)
                    r_acc += color[0]
                    g_acc += color[1]
                    b_acc += color[2]
                    a_acc += 255

            samples = SS * SS
            index = (py * size + px) * 4
            if a_acc:
                covered = a_acc // 255
                pixels[index + 0] = r_acc // covered
                pixels[index + 1] = g_acc // covered
                pixels[index + 2] = b_acc // covered
                pixels[index + 3] = a_acc // samples
    return bytes(pixels)


def encode_png(size, pixels):
    raw = b"".join(
        b"\x00" + pixels[y * size * 4 : (y + 1) * size * 4] for y in range(size)
    )

    def chunk(kind, data):
        return (
            struct.pack(">I", len(data))
            + kind
            + data
            + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
        )

    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def build(path):
    images = [(size, encode_png(size, render(size))) for size in SIZES]

    header = struct.pack("<HHH", 0, 1, len(images))
    directory = b""
    offset = 6 + 16 * len(images)
    body = b""

    for size, png in images:
        directory += struct.pack(
            "<BBBBHHII",
            0 if size >= 256 else size,
            0 if size >= 256 else size,
            0,
            0,
            1,
            32,
            len(png),
            offset,
        )
        body += png
        offset += len(png)

    with open(path, "wb") as handle:
        handle.write(header + directory + body)
    print(f"wrote {path} ({offset} bytes, {len(images)} sizes)")


if __name__ == "__main__":
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    build(os.path.join(root, "resources", "livewall.ico"))
