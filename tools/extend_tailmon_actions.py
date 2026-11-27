"""Add drawn transition frames to Tailmon routine sources."""

import argparse
from collections import defaultdict
import json
from pathlib import Path

from PIL import Image

from assemble_tailmon_routines import alpha_box, fit_halfway, is_clean, overlap, silhouette
from patch_tailmon_quarters import eye_x, keep_head_in_place, rgba_samples, write_clip


def fit_sequence(cels, before, after):
    if len(cels) == 1:
        return [fit_halfway(cels[0], before, after)]
    frames = [before]
    for index, cel in enumerate(cels, 1):
        frames.append(fit_halfway(cel, before, after, index / (len(cels) + 1)))
    frames.append(after)

    # Align against immediate neighbours as well as the original endpoints.
    # Keep the starting placement if aligning to a neighbour makes a bigger jump.
    for indices in (range(1, len(frames) - 1), range(len(frames) - 2, 0, -1)):
        for index in indices:
            left, current, right = frames[index - 1:index + 2]
            candidate = fit_halfway(cels[index - 1], left, right)
            a, b, c, d = (silhouette(frame) for frame in (left, right, current, candidate))
            if min(overlap(a, d), overlap(d, b)) > min(overlap(a, c), overlap(c, b)):
                frames[index] = candidate
    return frames[1:-1]


def fit_action_sequence(action, cels, before, after):
    frames = fit_sequence(cels, before, after)
    if action == "tail-windmill":
        anchor = (eye_x(before) + eye_x(after)) / 2
        return [keep_head_in_place(frame, anchor) for frame in frames]
    return frames


def extend_actions(animations, drawings, output):
    windows = defaultdict(list)
    for row in json.loads((drawings / "details.json").read_text(encoding="utf-8")):
        windows[row["action"]].append(row)
    output.mkdir(parents=True, exist_ok=True)
    for action, regions in sorted(windows.items()):
        path = animations / f"{action}.png"
        base = rgba_samples(path, regions[0].get("base_samples", 33))
        sequence = [base[0]]
        previous = 0
        for region in sorted(regions, key=lambda region: region["start"]):
            start, end, pngs = region["start"], region["end"], region["pngs"]
            if region.get("base_samples", 33) != len(base) or not previous <= start < end < len(base) or not pngs:
                raise ValueError(f"{action}: invalid or overlapping interval {start}-{end}")
            cels = []
            for name in pngs:
                with Image.open(drawings / name) as source:
                    cel = source.convert("RGBA")
                box = alpha_box(cel)
                if box is None or not is_clean(cel):
                    raise ValueError(f"{name}: expected a transparent animation drawing")
                cels.append(cel.crop(box))
            sequence.extend(base[previous + 1:start + 1])
            additions = fit_action_sequence(action, cels, base[start], base[end])
            sequence.extend(additions)
            sequence.append(base[end])
            previous = end
        sequence.extend(base[previous + 1:])
        write_clip(output / path.name, sequence)
    print(f"Extended {len(windows)} Tailmon actions")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ("animations", "drawings", "output"):
        parser.add_argument(argument, type=Path)
    extend_actions(**vars(parser.parse_args()))
