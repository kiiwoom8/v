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


def place_bridge(cel, before, after, phase=.5):
    endpoints = [alpha_box(image) for image in (before, after)]
    anchors = [box[0] + contact_x(image.crop(box)) for image, box in zip((before, after), endpoints)]
    width = before.width
    weights = (1 - phase, phase)
    floor = round(sum(box[3] * weight for box, weight in zip(endpoints, weights)))
    height = sum((box[3] - box[1]) * weight for box, weight in zip(endpoints, weights))
    scale = min(height / cel.height, (width - 6) / cel.width, (floor - 3) / cel.height)
    cel = cel.resize((round(cel.width * scale), round(cel.height * scale)), Image.Resampling.LANCZOS)
    x = round(sum(anchor * weight for anchor, weight in zip(anchors, weights)) - contact_x(cel))
    x = max(3, min(x, width - cel.width - 3))
    image = Image.new("RGBA", before.size)
    image.alpha_composite(cel, (x, floor - cel.height))
    return image


def midpoint_drawings(folder):
    """(action, interval) -> the drawn halfway pose of that interval. `midpoints.json` lists
    each drawing with its action and 1-based gap; interval 0 is neutral -> entry drawing."""
    drawings = {}
    for row in json.loads((folder / "midpoints.json").read_text(encoding="utf-8")):
        with Image.open(folder / row["png"]) as source:
            cel = source.convert("RGBA")
        drawings[(row["action"], row["gap"] - 1)] = cel.crop(alpha_box(cel))
    return drawings


def silhouette(image):
    return np.asarray(image.getchannel("A")) >= 128


def overlap(a, b):
    return np.count_nonzero(a & b) / max(1, np.count_nonzero(a | b))


def fit_halfway(cel, before, after, phase=.5):
    """A drawn halfway pose on the canvas, sized and placed to overlap both of its neighbours
    best: the drawings were made at their own scale, and the bridges' rule (the average box
    height of the endpoints) left some of them 10-20 % smaller than the cat on either side,
    so she shrank and grew back within a quarter second."""
    base = place_bridge(cel, before, after, phase)
    box = alpha_box(base)
    floor = box[3]
    height = box[3] - box[1]
    centre = (box[0] + box[2]) / 2
    ends = [silhouette(before), silhouette(after)]
    best = None
    for step in range(-6, 11):
        tall = round(height * (1 + step * .025))
        if tall < 8 or tall > floor - 3:
            continue
        sized = cel.resize((max(1, round(cel.width * tall / cel.height)), tall), Image.Resampling.LANCZOS)
        for dx in range(-12, 13, 2):
            x = round(centre - sized.width / 2) + dx
            if x < 3 or x + sized.width > before.width - 3:
                continue
            image = Image.new("RGBA", before.size)
            image.alpha_composite(sized, (x, floor - tall))
            shape = silhouette(image)
            score = min(overlap(shape, end) for end in ends)
            if best is None or score > best[0]:
                best = (score, image)
    return best[1] if best else base


def halfway_frames(rife, model, work, pairs):
    """RIFE's halfway frame of every (A, B) pair of same-sized drawings."""
    if not pairs:
        return []
    poses = np.stack([to_bgra(image) for pair in pairs for image in pair])
    keys = list(range(len(poses)))
    work.mkdir(parents=True, exist_ok=True)
    dark = infer(rife, model, work, poses, keys, 1, 0, pose_count=len(poses))
    light = infer(rife, model, work, poses, keys, 1, 255, pose_count=len(poses))
    # with one in-between, source i is output 2i + 1 and the frame after it 2i + 2
    return [to_rgba(recover_frame(dark / f"{4 * i + 2:06d}.png", light / f"{4 * i + 2:06d}.png"))
            for i in range(len(pairs))]


def to_bgra(image):
    return np.asarray(image.convert("RGBa"))[..., [2, 1, 0, 3]]


def to_rgba(bgra):
    rgba = bgra[..., [2, 1, 0, 3]].copy()
    height, width = rgba.shape[:2]
    return Image.frombytes("RGBa", (width, height), rgba.tobytes()).convert("RGBA")


def is_clean(candidate):
    """False when the interpolator drew overlapping translucent bodies (a ghost)."""
    alpha = np.asarray(candidate.getchannel("A"))
    haze = np.count_nonzero((alpha > 23) & (alpha < 175))
    solid = np.count_nonzero(alpha >= 225)
    return bool(solid) and haze * 100 <= (haze + solid) * 17


def contact_point(image):
    """Where the body meets the floor: what travels in a hop, a pounce or a backstep."""
    box = alpha_box(image)
    return box[0] + contact_x(image.crop(box)), box[3]


def clean_inbetween(candidate, before, after, phase):
    if is_clean(candidate):
        return candidate
    # Large pose changes can fool the interpolator into drawing overlapping
    # translucent bodies. Use a clean key drawing and carry its position
    # through those fast intervals, preserving the rendered silhouette.
    # The carried position is the floor contact, not the box centre: lying
    # down or rising moves the box while the body stays where it is, and
    # carrying the box slid a still drawing sideways (2026-11-28).
    previous_box, next_box = alpha_box(before), alpha_box(after)
    (x0, y0), (x1, y1) = contact_point(before), contact_point(after)
    after_midpoint = phase >= .5
    fraction = phase - after_midpoint
    left, top, right, bottom = next_box if after_midpoint else previous_box
    dx = max(1 - left, min(round((x1 - x0) * fraction), before.width - right - 1))
    dy = max(1 - top, min(round((y1 - y0) * fraction), before.height - bottom - 1))
    clean = Image.new("RGBA", before.size)
    clean.paste(after if after_midpoint else before, (dx, dy))
    return clean


def assemble(base, sources, rife, model, work, output, bridges=None, midpoints=None):
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
    # An interval RIFE cannot follow (a roll, a bow, a pounce) gets a drawn halfway pose:
    # RIFE then only covers the two short steps either side of it.
    halfway = {}
    if midpoints:
        drawn = midpoint_drawings(midpoints)
        for name, first, last in clips:
            for position in range(first, last):
                cel = drawn.pop((name, position - first), None)
                if cel is not None:
                    halfway[position] = fit_halfway(cel, frames[keys[position]], frames[keys[position + 1]])
        if drawn:
            raise ValueError(f"midpoint drawings for no interval: {sorted(drawn)}")
        order = sorted(halfway)
        steps = halfway_frames(rife, model, work / "midpoints", [
            pair for p in order for pair in ((frames[keys[p]], halfway[p]), (halfway[p], frames[keys[p + 1]]))])
        quarter = {p: steps[2 * k:2 * k + 2] for k, p in enumerate(order)}
    for name, first, last in clips:
        sequence = [neutral]
        for position in range(first, last):
            before, after = frames[keys[position]], frames[keys[position + 1]]
            if position in halfway and INBETWEENS == 3:
                middle = halfway[position]
                first_step, second_step = quarter[position]
                # a quarter step that still ghosts holds the drawing it leaves
                sequence.extend([first_step if is_clean(first_step) else before, middle,
                                 second_step if is_clean(second_step) else middle, after])
                continue
            candidates = []
            for j in range(1, INBETWEENS + 1):
                at = position * (INBETWEENS + 1) + j + 1
                filename = f"{at:06d}.png"
                candidates.append(to_rgba(recover_frame(dark / filename, light / filename)))
            # When the halfway frame is clean, a ghosted frame next to a drawing holds that
            # drawing; only an interval whose middle ghosts cuts between the drawings. Mixing
            # copies of the drawings with interpolated frames inside one interval flickered.
            middle_clean = is_clean(candidates[INBETWEENS // 2])
            for j, candidate in enumerate(candidates, 1):
                phase = j / (INBETWEENS + 1)
                if is_clean(candidate) or not middle_clean:
                    sequence.append(clean_inbetween(candidate, before, after, phase))
                else:
                    sequence.append(after if phase > .5 else before)
            sequence.append(after)
        path = output / f"{name}.png"
        sequence[0].save(path, save_all=True, append_images=sequence[1:], duration=FRAME_MS if bridges else 50, loop=0)
    print(f"Assembled {len(clips)} APNG animations from {len(frames) - 1} GPT Image drawings")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("base", "sources", "rife", "model", "work", "output"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--bridges", type=Path)
    parser.add_argument("--midpoints", type=Path, help="drawn halfway poses (midpoints.json)")
    assemble(**vars(parser.parse_args()))
