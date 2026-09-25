#!/usr/bin/env python3
# Makes a synthetic test scene with a known depth map (white is near):
#
# - far: a textured wall with small bright lights, for bokeh
# - middle: a sharp subject (checkerboard disc), where the focus goes
# - near: a bar across the left, in front of everything
# - bottom: a ground plane whose depth changes smoothly, to show banding
#
# Writes scene.png and depth.png to the folder given (default: tests/output).

import os
import sys

import numpy as np
from PIL import Image

W, H = 800, 533
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "output")
os.makedirs(out, exist_ok=True)

rng = np.random.default_rng(1)
y, x = np.mgrid[0:H, 0:W].astype(np.float32)

# far wall: soft blue-grey texture with a fine grid
img = np.empty((H, W, 3), np.float32)
img[..., 0] = 0.25 + 0.05 * np.sin(x / 7.0)
img[..., 1] = 0.30 + 0.05 * np.sin(y / 5.0)
img[..., 2] = 0.40
grid = ((x.astype(int) % 24) < 2) | ((y.astype(int) % 24) < 2)
img[grid] = [0.15, 0.15, 0.2]
depth = np.full((H, W), 0.1, np.float32)

# lights on the wall
for _ in range(40):
    cx, cy = rng.uniform(0, W), rng.uniform(0, H * 0.6)
    col = rng.choice([[1, 0.9, 0.6], [1, 1, 1], [0.6, 0.8, 1]])
    d2 = (x - cx) ** 2 + (y - cy) ** 2
    img[d2 < 2.5 ** 2] = col

# ground plane: depth from far (0.1) at the horizon to near (0.9) at the bottom
horizon = int(H * 0.62)
t = np.clip((y - horizon) / (H - horizon), 0, 1)
ground = y >= horizon
stripes = ((x - W / 2) * (0.3 + t) / 20.0).astype(int) % 2 == 0
img[ground & stripes] = [0.45, 0.35, 0.2]
img[ground & ~stripes] = [0.3, 0.22, 0.12]
depth[ground] = (0.1 + 0.8 * t)[ground]

# subject: a checkerboard disc at depth 0.5
cx, cy, r = W * 0.58, H * 0.45, H * 0.22
disc = (x - cx) ** 2 + (y - cy) ** 2 < r ** 2
check = ((x // 12).astype(int) + (y // 12).astype(int)) % 2 == 0
img[disc & check] = [0.9, 0.9, 0.85]
img[disc & ~check] = [0.8, 0.1, 0.1]
depth[disc] = 0.5

# near bar: dark green with light stripes, at depth 0.95
bar = (x > W * 0.12) & (x < W * 0.22)
img[bar] = [0.1, 0.35, 0.1]
img[bar & (((y // 10).astype(int) % 3) == 0)] = [0.7, 0.9, 0.6]
depth[bar] = 0.95

Image.fromarray((np.clip(img, 0, 1) ** (1 / 2.2) * 255 + 0.5).astype(np.uint8)).save(
    os.path.join(out, "scene.png"))
Image.fromarray((depth * 255 + 0.5).astype(np.uint8), "L").save(os.path.join(out, "depth.png"))
print("wrote", os.path.join(out, "scene.png"), "and depth.png")
