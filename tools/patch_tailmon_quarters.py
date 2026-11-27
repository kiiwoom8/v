"""Fit drawn Tailmon quarter poses to the timeline and update the affected APNGs."""

import argparse
from collections import defaultdict
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageSequence

from assemble_tailmon_routines import alpha_box, fit_halfway, is_clean
from pack_tailmon_routines import FRAME_MS


def rgba_samples(path, expected=33):
    with Image.open(path) as clip:
        samples = []
        for frame in ImageSequence.Iterator(clip):
            duration = frame.info.get("duration", 0)
            if duration <= 0 or duration % FRAME_MS:
                raise ValueError(f"{path.name}: expected {FRAME_MS} ms samples")
            samples.extend([frame.convert("RGBA")] * (int(duration) // FRAME_MS))
    if len(samples) != expected or samples[0].tobytes() != samples[-1].tobytes():
        raise ValueError(f"{path.name}: expected {expected} samples with the same seated endpoints")
    return samples


def eye_x(image):
    red, green, blue, alpha = np.moveaxis(np.asarray(image, dtype=np.int16), -1, 0)
    iris = (alpha > 200) & (green > 65) & (green > red + 22) & (blue > green + 10)
    _, x = np.where(iris)
    if not len(x):
        raise ValueError("Tailmon's eyes were not visible in the tail-windmill drawing")
    return np.median(x)


def keep_head_in_place(image, anchor):
    left, _, right, _ = alpha_box(image)
    shift = max(3 - left, min(round(anchor - eye_x(image)), image.width - 3 - right))
    positioned = Image.new("RGBA", image.size)
    positioned.paste(image, (shift, 0))
    return positioned


def write_clip(path, frames):
    temporary = path.with_suffix(".png.tmp")
    frames[0].save(temporary, format="PNG", save_all=True, append_images=frames[1:],
                   duration=FRAME_MS, loop=0)
    temporary.replace(path)


def patch_quarters(animations, drawings, output):
    paths = {path.stem: path for path in animations.glob("*.png")}
    replacements = defaultdict(dict)
    for row in json.loads((drawings / "quarters.json").read_text(encoding="utf-8")):
        action, sample = row["action"], row["sample"]
        tail_turn = action == "tail-windmill" and sample in (18, 22)
        if action not in paths or (sample not in range(1, 32, 2) and not tail_turn) or sample in replacements[action]:
            raise ValueError(f"invalid or repeated quarter sample: {action} {sample}")
        with Image.open(drawings / row["png"]) as source:
            image = source.convert("RGBA")
        box = alpha_box(image)
        if box is None or not is_clean(image):
            raise ValueError(f"{row['png']}: expected one opaque drawing on transparent alpha")
        replacements[action][sample] = image.crop(box)

    output.mkdir(parents=True, exist_ok=True)
    # In the windmill, the tail sweeps across the floor while the head stays
    # seated. The changing silhouette had dragged the fitted head sideways.
    if "tail-windmill" in paths:
        replacements.setdefault("tail-windmill", {})
    for action, cels in sorted(replacements.items()):
        frames = rgba_samples(paths[action])
        head_anchor = None
        if action == "tail-windmill":
            head_anchor = (eye_x(frames[8]) + eye_x(frames[16])) / 2
            for sample in range(9, 16):
                frames[sample] = keep_head_in_place(frames[sample], head_anchor)
        for sample, cel in cels.items():
            before, after = frames[sample - 1], frames[sample + 1]
            frame = fit_halfway(cel, before, after)
            if head_anchor is not None:
                frame = keep_head_in_place(frame, head_anchor)
            frames[sample] = frame
        write_clip(output / paths[action].name, frames)
    print(f"Placed {sum(map(len, replacements.values()))} quarter drawings in {len(replacements)} clips")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ("animations", "drawings", "output"):
        parser.add_argument(argument, type=Path)
    patch_quarters(**vars(parser.parse_args()))
