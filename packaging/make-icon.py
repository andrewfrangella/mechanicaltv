#!/usr/bin/env python3
"""Draw the launcher icon: a Nipkow disk with its spiral of holes, lit by the LED behind it.

Writes packaging/mechanical-tv.png (256x256, transparent background). Needs Pillow.
    python3 packaging/make-icon.py
"""
import math
import os
import sys

from PIL import Image, ImageDraw

SIZE = 256
HOLES = 32            # one scan line each, like the real disk
SCALE = 4             # draw at 4x then downsample for smooth edges


def main(path: str) -> None:
    s = SIZE * SCALE
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = s / 2
    r = s * 0.47
    # LED glow behind the disk
    for i in range(12, 0, -1):
        k = i / 12
        d.ellipse([c - r * (1 + 0.1 * k)] * 2 + [c + r * (1 + 0.1 * k)] * 2,
                  fill=(255, 150, 40, int(28 * (1 - k))))
    # disk body and rim
    d.ellipse([c - r, c - r, c + r, c + r], fill=(34, 36, 42, 255), outline=(120, 124, 132, 255), width=3 * SCALE)
    # spiral of holes: radius shrinks one scan-line pitch per hole over one turn
    r_out, r_in = r * 0.88, r * 0.42
    for n in range(HOLES):
        a = -math.pi / 2 + 2 * math.pi * n / HOLES
        rad = r_out - (r_out - r_in) * n / (HOLES - 1)
        hr = r * 0.022 + r * 0.006 * (1 - n / HOLES)
        x, y = c + rad * math.cos(a), c + rad * math.sin(a)
        d.ellipse([x - hr, y - hr, x + hr, y + hr], fill=(255, 196, 90, 255))
    # hub
    d.ellipse([c - r * 0.1, c - r * 0.1, c + r * 0.1, c + r * 0.1], fill=(150, 152, 160, 255))
    d.ellipse([c - r * 0.035, c - r * 0.035, c + r * 0.035, c + r * 0.035], fill=(60, 62, 70, 255))
    img = img.resize((SIZE, SIZE), Image.LANCZOS)
    img.save(path, optimize=True)
    print(f"wrote {path} ({os.path.getsize(path)} bytes)")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "mechanical-tv.png"))
