"""Re-encode the bundled Tailmon sheets without changing visible pixels."""

import argparse
from pathlib import Path

import numpy as np
from PIL import Image


def compact(source, output):
    output.mkdir(parents=True, exist_ok=True)
    saved = 0
    for name in ("poses", "movement", "rest", "reactions", "walk", "attention", "idle", "interaction"):
        path = source / f"{name}.png"
        bundled = output / path.name
        with Image.open(path) as image:
            pixels = np.array(image.convert("RGBA"))
        pixels[pixels[:, :, 3] == 0, :3] = 0
        Image.fromarray(pixels).save(bundled, optimize=True)
        saved += path.stat().st_size - bundled.stat().st_size
    print(f"Saved {saved:,} embedded bytes by recompressing eight sheets")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    compact(args.source, args.output)
