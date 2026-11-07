#!/usr/bin/env python3
"""Import ROC.ico/TFT.ico from the Windows game directory into Android launcher
resources.

The game ships one icon per mode.  Android cannot use .ico directly, so this
script extracts the largest frame and writes both legacy mipmaps and the
foreground layer for an adaptive icon.  The generated PNGs are checked in; the
APK build does not need the Windows game directory.
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:  # pragma: no cover - the message is the useful part
    sys.exit("Pillow is required to import the .ico files: pip install pillow")

# Legacy launcher icon sizes, in pixels.
LEGACY_SIZES = {
    "ldpi": 36,
    "mdpi": 48,
    "hdpi": 72,
    "xhdpi": 96,
    "xxhdpi": 144,
    "xxxhdpi": 192,
}

# Adaptive-icon layers use a 108 dp canvas.  The visible mask is smaller, so
# keep the framed portrait inside the safe area instead of cropping its border.
FOREGROUND_SIZES = {
    "ldpi": 81,
    "mdpi": 108,
    "hdpi": 162,
    "xhdpi": 216,
    "xxhdpi": 324,
    "xxxhdpi": 432,
}
FOREGROUND_SCALE = 0.72

MODES = {
    "roc": "ROC.ico",
    "tft": "TFT.ico",
}


def load_largest_frame(path: Path) -> Image.Image:
    image = Image.open(path)
    if not hasattr(image, "ico"):
        raise SystemExit(f"{path} is not an ICO file")
    largest = max(image.ico.sizes())
    image.size = largest
    return image.convert("RGBA")


def resize(image: Image.Image, size: int) -> Image.Image:
    return image.resize((size, size), Image.Resampling.LANCZOS)


def write_legacy(icon: Image.Image, res_dir: Path, mode: str) -> int:
    resources = 0
    name = f"ic_launcher_{mode}.png"
    for density, size in LEGACY_SIZES.items():
        target = res_dir / f"mipmap-{density}" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        resize(icon, size).save(target, optimize=True)
        resources += 1
    return resources


def write_foreground(icon: Image.Image, res_dir: Path, mode: str) -> int:
    resources = 0
    name = f"ic_launcher_{mode}_foreground.png"
    for density, canvas_size in FOREGROUND_SIZES.items():
        canvas = Image.new("RGBA", (canvas_size, canvas_size), (0, 0, 0, 0))
        art_size = max(1, round(canvas_size * FOREGROUND_SCALE))
        art = resize(icon, art_size)
        offset = ((canvas_size - art_size) // 2, (canvas_size - art_size) // 2)
        canvas.alpha_composite(art, offset)
        target = res_dir / f"mipmap-{density}" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        canvas.save(target, optimize=True)
        resources += 1
    return resources


def main() -> int:
    default_game_dir = os.environ.get("WC3_GAME_DIR", r"E:\Games\Warcraft III")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-dir", default=default_game_dir,
                        help="directory containing ROC.ico and TFT.ico")
    parser.add_argument("--res-dir", default="apps/android_gameactivity/app/src/main/res",
                        help="Android main source-set res directory")
    args = parser.parse_args()

    game_dir = Path(args.game_dir)
    res_dir = Path(args.res_dir)
    if not res_dir.is_dir():
        raise SystemExit(f"resource directory not found: {res_dir}")

    total = 0
    for mode, filename in MODES.items():
        source = game_dir / filename
        if not source.is_file():
            raise SystemExit(f"missing game icon: {source}")
        icon = load_largest_frame(source)
        total += write_legacy(icon, res_dir, mode)
        total += write_foreground(icon, res_dir, mode)
        print(f"[icon] {source} -> {mode} ({icon.width}x{icon.height})")

    print(f"[icon] wrote {total} PNGs under {res_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
