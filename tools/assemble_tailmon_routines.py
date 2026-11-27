"""Assemble Tailmon key drawings and GPT Image bridges into transparent APNG clips."""

import argparse
import json
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


def gutters(profile, divisions):
    margin = len(profile) // (4 * divisions)
    bounds = [0]
    for i in range(1, divisions):
        center = len(profile) * i // divisions
        bound = min(range(center - margin, center + margin + 1),
                    key=lambda at: (profile[at - 1] + profile[at], abs(at - center)))
        bounds.append(bound)
    return [*bounds, len(profile)]


def split_sheet(path, divisions):
    with Image.open(path) as source:
        sheet = source.convert("RGBA")
    cells = []
    opaque = np.asarray(sheet.getchannel("A")) >= 100
    rows = gutters(opaque.sum(axis=1), divisions)
    for top, bottom in zip(rows, rows[1:]):
        columns = gutters(opaque[top:bottom].sum(axis=0), divisions)
        for left, right in zip(columns, columns[1:]):
            cell = sheet.crop((left, top, right, bottom))
            box = alpha_box(cell)
            if box is None or box[0] == 0 or box[1] == 0 or box[2] == cell.width or box[3] == cell.height:
                raise ValueError(f"{path.name}: cell {len(cells) + 1} is empty, opaque, or touches the grid boundary")
            cells.append(cell.crop(box))
    return cells


def key_cels(path, neutral):
    cells = split_sheet(path, 2)
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


def bridge_rows(folder):
    groups = json.loads((folder / "groups.json").read_text(encoding="utf-8"))
    rows = {}
    for group, actions in groups.items():
        if len(actions) != 3 or len(set(actions)) != 3 or set(actions) & rows.keys():
            raise ValueError(f"{group}: expected three distinct action names")
        cells = split_sheet(folder / f"{group}.png", 3)
        for i, action in enumerate(actions):
            rows[action] = cells[3 * i:3 * i + 3]
    return rows


def place_bridge(cel, before, after):
    endpoints = [alpha_box(image) for image in (before, after)]
    anchors = [box[0] + contact_x(image.crop(box)) for image, box in zip((before, after), endpoints)]
    width = before.width
    floor = round(sum(box[3] for box in endpoints) / 2)
    height = sum(box[3] - box[1] for box in endpoints) / 2
    scale = min(height / cel.height, (width - 6) / cel.width, (floor - 3) / cel.height)
    cel = cel.resize((round(cel.width * scale), round(cel.height * scale)), Image.Resampling.LANCZOS)
    x = round(sum(anchors) / 2 - contact_x(cel))
    x = max(3, min(x, width - cel.width - 3))
    image = Image.new("RGBA", before.size)
    image.alpha_composite(cel, (x, floor - cel.height))
    return image


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


def assemble(base, sources, rife, model, work, output, bridges=None):
    pack = MotionPack(base)
    with CabinetCodec(5, decode=True) as decoder:
        rgba = pack.frame(decoder, 112)
    neutral = to_rgba(rgba[..., [2, 1, 0, 3]])
    frames = [neutral]
    keys, clips = [0], []
    paths = sorted(sources.glob("*.png"))
    if not paths:
        raise ValueError("no action sheets to assemble")
    bridge_art = bridge_rows(bridges) if bridges else {}
    if bridges and bridge_art.keys() != {path.stem for path in paths}:
        raise ValueError("bridge manifest and action sheets must name the same actions")
    for path in paths:
        first, second, third, fourth = key_cels(path, neutral)
        drawings = [first, second, third, fourth]
        if bridges:
            enter, middle, recover = bridge_art[path.stem]
            drawings = [place_bridge(enter, neutral, first), first, second,
                        place_bridge(middle, second, third), third, fourth,
                        place_bridge(recover, fourth, neutral)]
        start = len(keys) - 1
        keys.extend(range(len(frames), len(frames) + len(drawings)))
        keys.append(0)
        frames.extend(drawings)
        clips.append((path.stem, start, len(keys) - 1))
    pixels = np.stack([to_bgra(frame) for frame in frames])
    work.mkdir(parents=True, exist_ok=True)
    dark = infer(rife, model, work, pixels, keys, INBETWEENS, 0, pose_count=len(frames))
    light = infer(rife, model, work, pixels, keys, INBETWEENS, 255, pose_count=len(frames))
    output.mkdir(parents=True, exist_ok=True)
    for name, first, last in clips:
        sequence = [neutral]
        for position in range(first, last):
            before, after = frames[keys[position]], frames[keys[position + 1]]
            for j in range(1, INBETWEENS + 1):
                at = position * (INBETWEENS + 1) + j + 1
                filename = f"{at:06d}.png"
                candidate = to_rgba(recover_frame(dark / filename, light / filename))
                sequence.append(clean_inbetween(candidate, before, after, j / (INBETWEENS + 1)))
            sequence.append(after)
        path = output / f"{name}.png"
        sequence[0].save(path, save_all=True, append_images=sequence[1:], duration=FRAME_MS if bridges else 50, loop=0)
    print(f"Assembled {len(clips)} APNG animations from {len(frames) - 1} GPT Image drawings")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("base", "sources", "rife", "model", "work", "output"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--bridges", type=Path)
    assemble(**vars(parser.parse_args()))
