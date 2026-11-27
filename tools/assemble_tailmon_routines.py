"""Assemble GPT Image's 2 x 2 Tailmon key drawings into transparent APNG clips."""

import argparse
from pathlib import Path

import numpy as np
from PIL import Image

from bake_tailmon_motion import infer, recover_frame
from pack_tailmon_routines import CabinetCodec, FRAME_MS, MotionPack


INBETWEENS = 3
TRAJECTORIES = {
    # Extra displacement relative to the desktop contact point (x, y).
    "happy-hop": [(0, 0), (0, -13), (0, -7), (0, 0)],
    "pounce": [(0, 0), (-7, -9), (-12, 0), (-4, 0)],
    "surprised-backstep": [(0, 0), (3, 0), (8, 0), (3, 0)],
    "side-tumble": [(0, 0), (3, 0), (7, 0), (3, 0)],
}


def alpha_box(image):
    return image.getchannel("A").point(lambda value: 255 if value >= 100 else 0).getbbox()


def contact_x(image):
    alpha = np.asarray(image.getchannel("A"))
    bottom_band = alpha[-max(4, alpha.shape[0] // 12):]
    _, x = np.where(bottom_band >= 100)
    if not len(x):
        raise ValueError("sprite has no visible contact pixels")
    return float(np.median(x))


def gutter(profile):
    middle = len(profile) // 2
    margin = len(profile) // 8
    return min(range(middle - margin, middle + margin + 1),
               key=lambda at: (profile[at - 1] + profile[at], abs(at - middle)))


def key_cels(path, neutral):
    with Image.open(path) as source:
        sheet = source.convert("RGBA")
    cells = []
    opaque = np.asarray(sheet.getchannel("A")) >= 100
    middle_y = gutter(opaque.sum(axis=1))
    for top, bottom in ((0, middle_y), (middle_y, sheet.height)):
        middle_x = gutter(opaque[top:bottom].sum(axis=0))
        for left, right in ((0, middle_x), (middle_x, sheet.width)):
            cell = sheet.crop((left, top, right, bottom))
            box = alpha_box(cell)
            if box is None or box[0] == 0 or box[1] == 0 or box[2] == cell.width or box[3] == cell.height:
                raise ValueError(f"{path.name}: cell {len(cells) + 1} is empty, opaque, or touches the grid boundary")
            cells.append(cell.crop(box))

    left, top, right, floor = alpha_box(neutral)
    anchor = left + contact_x(neutral.crop((left, top, right, floor)))
    scale = min((floor - top) / max(cell.height for cell in cells),
                (neutral.width - 8) / max(cell.width for cell in cells))
    trajectory = TRAJECTORIES.get(path.stem, [(0, 0)] * len(cells))
    for cell, (dx, dy) in zip(cells, trajectory):
        resized = cell.resize((round(cell.width * scale), round(cell.height * scale)), Image.Resampling.LANCZOS)
        x = round(anchor - contact_x(resized)) + dx
        y = floor - resized.height + dy
        x = max(3, min(x, neutral.width - resized.width - 3))
        y = max(3, y)
        canvas = Image.new("RGBA", neutral.size)
        canvas.alpha_composite(resized, (x, y))
        yield canvas


def to_bgra(image):
    return np.asarray(image.convert("RGBa"))[..., [2, 1, 0, 3]]


def to_rgba(bgra):
    rgba = bgra[..., [2, 1, 0, 3]].copy()
    height, width = rgba.shape[:2]
    return Image.frombytes("RGBa", (width, height), rgba.tobytes()).convert("RGBA")


def clean_inbetween(candidate, before, after, phase):
    alpha = np.asarray(candidate.getchannel("A"))
    haze = np.count_nonzero((alpha > 23) & (alpha < 175))
    solid = np.count_nonzero(alpha >= 225)
    if solid and haze * 100 <= (haze + solid) * 17:
        return candidate
    # Large pose changes can fool the interpolator into drawing overlapping
    # translucent bodies. Use a clean key drawing and carry its position
    # through those fast intervals, preserving the rendered silhouette.
    previous_box, next_box = alpha_box(before), alpha_box(after)
    x0, y0 = (previous_box[0] + previous_box[2]) / 2, previous_box[3]
    x1, y1 = (next_box[0] + next_box[2]) / 2, next_box[3]
    after_midpoint = phase >= .5
    fraction = phase - after_midpoint
    left, top, right, bottom = next_box if after_midpoint else previous_box
    dx = max(1 - left, min(round((x1 - x0) * fraction), before.width - right - 1))
    dy = max(1 - top, min(round((y1 - y0) * fraction), before.height - bottom - 1))
    clean = Image.new("RGBA", before.size)
    clean.paste(after if after_midpoint else before, (dx, dy))
    return clean


def assemble(base, sources, rife, model, work, output):
    pack = MotionPack(base)
    with CabinetCodec(5, decode=True) as decoder:
        rgba = pack.frame(decoder, 112)
    neutral = to_rgba(rgba[..., [2, 1, 0, 3]])
    frames = [neutral]
    keys, clips = [0], []
    for path in sorted(sources.glob("*.png")):
        positions = list(range(len(frames), len(frames) + 4))
        frames.extend(key_cels(path, neutral))
        clips.append((path.stem, len(keys) - 1))
        keys.extend([*positions, 0])
    if len(frames) > 144 or not clips:
        raise ValueError("assemble between one and 35 sheets at a time")
    pixels = np.stack([to_bgra(frame) for frame in frames])
    work.mkdir(parents=True, exist_ok=True)
    dark = infer(rife, model, work, pixels, keys, INBETWEENS, 0)
    light = infer(rife, model, work, pixels, keys, INBETWEENS, 255)
    output.mkdir(parents=True, exist_ok=True)
    for name, first in clips:
        sequence = [neutral]
        for position in range(first, first + 5):
            before, after = frames[keys[position]], frames[keys[position + 1]]
            for j in range(1, INBETWEENS + 1):
                at = position * (INBETWEENS + 1) + j + 1
                filename = f"{at:06d}.png"
                candidate = to_rgba(recover_frame(dark / filename, light / filename))
                sequence.append(clean_inbetween(candidate, before, after, j / (INBETWEENS + 1)))
            sequence.append(after)
        path = output / f"{name}.png"
        sequence[0].save(path, save_all=True, append_images=sequence[1:], duration=FRAME_MS, loop=0)
    print(f"Assembled {len(clips)} APNG animations from {len(frames) - 1} GPT Image drawings")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("base", "sources", "rife", "model", "work", "output"):
        parser.add_argument(name, type=Path)
    assemble(**vars(parser.parse_args()))
