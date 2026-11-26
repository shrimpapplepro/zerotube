#!/usr/bin/env python3
"""Draw the ZeroTube icon set: a red rounded play button on YouTube's dark
background. The CardputerZero spec asks for 256x256; APPLaunch draws icons at
100px (centre tile) and 80px (side tiles), so all three are produced.

    python3 tools/make-icon.py      (needs Pillow)
"""
from PIL import Image, ImageDraw
import os
import sys

repo = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUTPUTS = [("assets/images/zerotube.png", 256), ("assets/images/zerotube_100.png", 100),
           ("assets/images/zerotube_80.png", 80)]

SIZE = 1024
img = Image.new("RGBA", (SIZE, SIZE), (15, 15, 15, 255))
d = ImageDraw.Draw(img)
w, h = 640, 450
x0, y0 = (SIZE - w) // 2, (SIZE - h) // 2
d.rounded_rectangle([x0, y0, x0 + w, y0 + h], radius=130, fill=(255, 0, 51, 255))
cx, cy, r = SIZE // 2 + 18, SIZE // 2, 120
d.polygon([(cx - r * 0.8, cy - r), (cx - r * 0.8, cy + r), (cx + r, cy)], fill=(255, 255, 255, 255))
for path, size in OUTPUTS:
    img.resize((size, size), Image.LANCZOS).save(os.path.join(repo, path))
print("wrote", ", ".join(p for p, _ in OUTPUTS))
