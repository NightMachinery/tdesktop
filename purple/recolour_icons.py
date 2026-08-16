#!/usr/bin/env python3
"""Recolours the Telegram logo assets from blue to purple, in place.

Run after upstream changes the icon artwork:

    python3 purple/recolour_icons.py

Requires Pillow and NumPy. The transform is a pure hue rotation, so the white
paper plane (saturation 0) and the alpha channel are untouched; only the blue
disc moves. Re-running on already-purple files would rotate them again, so
restore the originals first (git checkout) if you need to redo it.
"""

import glob
import os
import sys

import numpy as np
from PIL import Image

SOURCE_HUE = 203.0
TARGET_HUE = 277.0


def recolour(image, target_hue=TARGET_HUE, source_hue=SOURCE_HUE):
    arr = np.asarray(image.convert("RGBA")).astype(np.float32) / 255.0
    rgb, alpha = arr[..., :3], arr[..., 3:]

    mx = rgb.max(axis=-1)
    mn = rgb.min(axis=-1)
    diff = mx - mn
    value = mx
    saturation = np.where(mx > 0, diff / np.where(mx > 0, mx, 1), 0)

    safe = np.where(diff > 0, diff, 1)
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    hue = np.zeros_like(mx)
    hue = np.where(mx == r, ((g - b) / safe) % 6, hue)
    hue = np.where(mx == g, ((b - r) / safe) + 2, hue)
    hue = np.where(mx == b, ((r - g) / safe) + 4, hue)
    hue = np.where(diff > 0, hue * 60.0, 0.0)
    hue = (hue + (target_hue - source_hue)) % 360.0

    c = value * saturation
    x = c * (1 - np.abs((hue / 60.0) % 2 - 1))
    m = value - c
    z = np.zeros_like(hue)
    segment = (hue / 60.0).astype(int) % 6
    candidates = np.stack([
        np.stack([c, x, z], axis=-1),
        np.stack([x, c, z], axis=-1),
        np.stack([z, c, x], axis=-1),
        np.stack([z, x, c], axis=-1),
        np.stack([x, z, c], axis=-1),
        np.stack([c, z, x], axis=-1),
    ])
    out = np.take_along_axis(candidates, segment[None, ..., None], axis=0)[0]
    out = np.concatenate([out + m[..., None], alpha], axis=-1)
    return Image.fromarray((np.clip(out, 0, 1) * 255).round().astype(np.uint8), "RGBA")


def targets(root):
    found = []
    for pattern in (
            "Telegram/Telegram/Images.xcassets/Icon.iconset/*.png",
            "Telegram/Telegram/Images.xcassets/Icon.appiconset/*.png"):
        found += glob.glob(os.path.join(root, pattern))
    for size in (16, 32, 48, 64, 128, 256, 512):
        for suffix in ("", "@2x"):
            found.append(
                os.path.join(root, f"Telegram/Resources/art/icon{size}{suffix}.png"))
    for name in ("icon_round512@2x.png", "logo_256.png", "logo_256_no_margin.png"):
        found.append(os.path.join(root, "Telegram/Resources/art", name))
    return sorted({path for path in found if os.path.exists(path)})


def main():
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    paths = targets(root)
    if not paths:
        print("no icon assets found under", root)
        return 1
    for path in paths:
        recolour(Image.open(path)).save(path)
        print("recoloured", os.path.relpath(path, root))
    print(len(paths), "files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
