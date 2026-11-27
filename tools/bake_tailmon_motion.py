"""Build the embedded Tailmon frames from the normalized pose export and RIFE 4.6."""

import argparse
import ctypes as ct
from collections import defaultdict
from pathlib import Path
import re
import struct
import subprocess

import numpy as np
from PIL import Image


POSES = 144
FRAMES_PER_BLOCK = 12
MOTION_WIDTH = 146
SHEETS = dict(WALK_FIRST=16, REST_FIRST=32, COMMAND_FIRST=48, WALK_DETAIL_FIRST=64,
              ATTENTION_FIRST=80, SCROLL_DETAIL_FIRST=96, ACCENT_FIRST=96, IDLE_DETAIL_FIRST=112,
              INTERACTION_FIRST=128)
TIMELINE = Path(__file__).resolve().parents[1] / "secretary.c"


def transition_edges():
    edges = {}
    clips = {}

    def edge(a, b, count=3, opposite=False):
        a, b = sorted((a, b))
        if a == b and not opposite:
            return
        key = a, b + POSES * opposite
        edges[key] = max(edges.get(key, 0), count)

    def run(poses, loop=False, count=3):
        for a, b in zip(poses, poses[1:]):
            edge(a, b, count)
        if loop:
            edge(poses[-1], poses[0], count)

    source = TIMELINE.read_text(encoding="utf-8")
    for name, clip in re.findall(r"static const PoseStep (\w+)\[\] = \{(.*?)\};", source, re.S):
        poses = []
        for expression in re.findall(r"\{([^,]+),", clip):
            base, _, offset = expression.partition("+")
            base = base.strip()
            poses.append((int(base) if base.isdecimal() else SHEETS[base]) + int(offset or 0))
        clips[name] = poses
    # The player traverses these routes continuously over their authored time.
    # Pay for dense motion there and use shorter transitions for reactions,
    # which last only a handful of display refreshes.
    density = dict(falling_asleep=15, curling_up=15, loaf_breath=23,
                   falling_asleep_custom=23, curling_up_custom=23, lying_down=23, getting_up=23,
                   sleep_breath=47, sleep_shift=23, waking=23, idle_breath=23,
                   idle_blink=11, idle_slow_blink=11, idle_glance=15, idle_listen=11,
                   idle_ear_flick=7, idle_tail_sweep=15, idle_head_tilt=11,
                   turning_detail=23, turning_run=15, turning_walk=11, settling=23,
                   grooming=23, grooming_long=23, stretching=15, stretching_short=15,
                   stretching_custom=23, looking=11, looking_quick=11,
                   yawn_detail=11, grab_detail=7)
    for name, poses in clips.items():
        if name == "idle_custom":
            continue
        run(poses, name in {"idle", "grabbed", "grab_detail", "sleep_breath", "loaf_breath"}, density.get(name, 3))

    walk = re.search(r"walk_detail_cycle\[\] = \{([^}]+)\}", source)[1]
    walk_poses = [SHEETS["WALK_DETAIL_FIRST"] + int(cel) for cel in re.findall(r"\d+", walk)]
    run(walk_poses, True, 23)
    for first, length, count in ((16, 8, 7), (24, 4, 11), (80, 16, 7)):
        run(list(range(first, first + length)), True, count)
    for first in (48, 52, 56, 60):
        run(list(range(first, first + 4)))
    run([52, 53, 54, 53, 55])
    run([80, 81, 82, 83, 84, 85, 87, 90, 93, 95])

    for entry in (12, 32, 44, 52, 56, 60, 80, 120, 128, 136):
        edge(112, entry, 7 if entry == 128 else 3)
    edge(113, 32)
    for idle in (112, 113, 118):
        edge(idle, 31, 7, True)
        edge(idle, 31, 7)
    for exit_pose in (0, 4, 12, 15, 43, 47, 51, 55, 59, 63, 95, 127, 143):
        edge(exit_pose, 112, 7 if exit_pose in (43, 47, 143) else 3)
    edge(31, 112, 7, True)
    for phase in walk_poses:
        edge(phase, 28, 7)
        edge(phase, 80, 3, True)
        edge(phase, 128, 3, True)
    for phase in (24, 25, 26, 27):
        edge(phase, 28, 3)
    for phase in (16, 24, 64):
        edge(phase, 28, 7)
    edge(32, 40, 11)
    edge(32, 44, 7)
    edge(31, 80, 7, True)
    edge(53, 55, 7)
    for phase in range(128, 136):
        edge(phase, 142, 15 if phase in (128, 131, 132, 135) else 7)
        edge(phase, 136, 3)
    for first, last in ((28, 31), (40, 47), (120, 127), (136, 143)):
        for phase in range(first, last + 1):
            edge(phase, 128, 3, first == 28)
    for phase in (80, 85, 90, 95):
        edge(phase, 128)
    for phase in (28, 29, 30, 31):
        edge(phase, 80, 3, True)
        edge(phase, 112, 2, True)
    for phase in range(80, 96):
        edge(phase, 112)
    return edges


def arrange(edges):
    """Walk the undirected pose graph, minimizing unused interpolations between trails."""
    graph = defaultdict(set)
    for a, b in edges:
        graph[a].add(b)
        graph[b].add(a)
    keys, segments = [], {}
    for start in sorted(graph, key=lambda key: (len(graph[key]) % 2 == 0, key)):
        while graph[start]:
            a = start
            keys.append(a)
            while graph[a]:
                b = min(graph[a], key=lambda key: (len(graph[key]) == 1, key))
                graph[a].remove(b)
                graph[b].remove(a)
                segments[min(a, b), max(a, b)] = len(keys) - 1, a > b
                keys.append(b)
                a = b
    return keys, segments


class CabinetCodec:
    """The Windows compression API used by the embedded motion pack."""

    def __init__(self, algorithm, *, decode=False):
        lib = ct.WinDLL("cabinet.dll", use_last_error=True)
        create = lib.CreateDecompressor if decode else lib.CreateCompressor
        create.argtypes = [ct.c_ulong, ct.c_void_p, ct.POINTER(ct.c_void_p)]
        self.process = lib.Decompress if decode else lib.Compress
        self.process.argtypes = [ct.c_void_p, ct.c_void_p, ct.c_size_t, ct.c_void_p,
                                 ct.c_size_t, ct.POINTER(ct.c_size_t)]
        self.close = lib.CloseDecompressor if decode else lib.CloseCompressor
        self.close.argtypes = [ct.c_void_p]
        self.handle = ct.c_void_p()
        if not create(algorithm, None, ct.byref(self.handle)):
            raise ct.WinError(ct.get_last_error())

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close(self.handle)

    def convert(self, payload, capacity):
        output = ct.create_string_buffer(capacity)
        length = ct.c_size_t()
        if not self.process(self.handle, payload, len(payload), output, len(output), ct.byref(length)):
            raise ct.WinError(ct.get_last_error())
        return output.raw[:length.value]


def indexed_frames(rgba):
    indexed = Image.fromarray(rgba).quantize(colors=256, method=Image.Quantize.FASTOCTREE)
    colors = np.frombuffer(indexed.palette.palette, dtype=np.uint8).reshape(-1, 4)
    palette = np.zeros((256, 4), dtype=np.uint8)
    palette[:len(colors)] = np.minimum(colors[:, [2, 1, 0, 3]], colors[:, 3:4])
    return palette.tobytes(), np.asarray(indexed)


def accent_poses(poses):
    """Shape nearby poses from the supplied sheets, with the contact floor pinned."""
    # Each region is (center x/y, horizontal/vertical falloff, displacement x/y).
    # Coordinates are fractions of the normalized 205 x 246 source canvas.
    edits = [
        (33, [(0.45, 0.40, 0.28, 0.26, 0.026, 0.028), (0.72, 0.54, 0.23, 0.17, 0.025, 0.039)]),
        (34, [(0.44, 0.45, 0.29, 0.22, -0.010, 0.027), (0.72, 0.52, 0.20, 0.22, 0.017, 0.043)]),
        (35, [(0.45, 0.55, 0.35, 0.20, 0.017, -0.017), (0.70, 0.62, 0.20, 0.15, -0.029, 0.010)]),
        (36, [(0.44, 0.52, 0.28, 0.16, -0.013, -0.009), (0.74, 0.63, 0.18, 0.15, -0.013, 0.017)]),
        (34, [(0.48, 0.50, 0.39, 0.23, 0.002, -0.013), (0.71, 0.53, 0.23, 0.20, 0.015, -0.012)]),
        (112, [(0.63, 0.35, 0.27, 0.12, 0.036, -0.027)]),
        (112, [(0.32, 0.30, 0.16, 0.13, 0.029, 0.018), (0.66, 0.35, 0.23, 0.10, -0.026, 0.014)]),
        (112, [(0.77, 0.56, 0.23, 0.18, 0.039, -0.033)]),
        (112, [(0.77, 0.55, 0.23, 0.19, -0.046, 0.024)]),
        (112, [(0.43, 0.44, 0.31, 0.30, -0.032, 0.009), (0.59, 0.34, 0.18, 0.16, 0.004, 0.019)]),
        (41, [(0.36, 0.53, 0.38, 0.21, 0.054, -0.045), (0.31, 0.68, 0.22, 0.07, 0.049, 0.001),
              (0.73, 0.50, 0.23, 0.23, 0.008, 0.027)]),
    ]
    _, height, width, _ = poses.shape
    grid_y, grid_x = np.indices((height, width), dtype=np.float32)
    for index, (source, regions) in enumerate(edits, SHEETS["ACCENT_FIRST"]):
        x, y = grid_x.copy(), grid_y.copy()
        for cx, cy, rx, ry, dx, dy in regions:
            radius = ((grid_x / width - cx) / rx)**2 + ((grid_y / height - cy) / ry)**2
            weight = np.maximum(0, 1 - radius)**2
            x -= dx * width * weight
            y -= dy * height * weight
        x = x.clip(0, width - 1)
        y = y.clip(0, height - 1)
        left, top = x.astype(int), y.astype(int)
        right, bottom = np.minimum(left + 1, width - 1), np.minimum(top + 1, height - 1)
        fx, fy = (x - left)[..., None], (y - top)[..., None]
        cel = poses[source]
        poses[index] = np.rint((cel[top, left] * (1 - fx) + cel[top, right] * fx) * (1 - fy)
                              + (cel[bottom, left] * (1 - fx) + cel[bottom, right] * fx) * fy)


def infer(exe, model, work, poses, keys, count, background):
    source = work / f"source-{count}-{background}"
    output = work / f"frames-{count}-{background}"
    source.mkdir(exist_ok=True)
    output.mkdir(exist_ok=True)
    scale = count + 1
    marker = output / "complete.txt"
    if marker.is_file() and len(list(output.glob("*.png"))) == len(keys) * scale:
        return output
    if len(list(source.glob("*.png"))) > len(keys) or len(list(output.glob("*.png"))) > len(keys) * scale:
        raise ValueError(f"stale frames in {work}, supply a fresh work directory")
    for i, key in enumerate(keys):
        image = poses[key % POSES]
        if key >= POSES:
            image = image[:, ::-1]
        rgb = image[..., 2::-1].astype(np.uint16) + (255 - image[..., 3:4]).astype(np.uint16) * background // 255
        Image.fromarray(rgb.astype(np.uint8)).save(source / f"{i:06d}.png", compress_level=1)
    subprocess.run([str(exe), "-i", str(source), "-o", str(output), "-n", str(len(keys) * scale),
                    "-m", str(model), "-g", "0", "-j", "2:2:2", "-f", "%06d.png"], check=True)
    marker.touch()
    return output


def recover_frame(dark, light):
    black = np.asarray(Image.open(dark), dtype=np.int16)
    white = np.asarray(Image.open(light), dtype=np.int16)
    alpha = 255 - np.median(white - black, axis=2).astype(np.int16)
    alpha = np.where(alpha < 12, 0, np.where(alpha > 247, 255, np.clip(alpha, 0, 255)))
    rgb = np.minimum(black, alpha[..., None])
    return np.dstack((rgb[..., ::-1], alpha)).astype(np.uint8)


def reuse_frames(path, width, height, endpoints, edges):
    if path is None:
        return {}
    data = path.read_bytes()
    magic, old_width, old_height, entries, count = struct.unpack_from("<4sHHHH", data)
    if (magic, old_width, old_height) != (b"VTM2", width, height):
        raise ValueError("the reusable pack has a different format or canvas")
    offset_at = 12 + 8 * entries
    offsets = struct.unpack_from(f"<{count + 1}I", data, offset_at)
    start = offset_at + 4 * (count + 1)
    frames = [data[start + a:start + b] for a, b in zip(offsets, offsets[1:])]
    unchanged = {i for i, frame in enumerate(endpoints) if frames[i] == frame}
    reused = {}
    for a, b, first, length in struct.iter_unpack("<HHHH", data[12:offset_at]):
        if length == edges.get((a, b)) and a in unchanged and b % POSES in unchanged:
            reused[a, b] = frames[first:first + length]
    return reused


def unpack_frame(decoder, packet, width, height):
    data = decoder.convert(packet, 1024 + width * height)
    palette = np.frombuffer(data, dtype=np.uint8, count=1024).reshape(256, 4)
    indices = np.frombuffer(data, dtype=np.uint8, offset=1024).reshape(height, width)
    return palette[indices]


def resize_frame(bgra, width, height):
    rgba = np.ascontiguousarray(bgra[..., [2, 1, 0, 3]])
    image = Image.frombytes("RGBa", (rgba.shape[1], rgba.shape[0]), rgba.tobytes())
    return np.asarray(image.resize((width, height), Image.Resampling.LANCZOS))


def write_pack(output, poses, edges, packets):
    # Keep RIFE's motion estimates as anchors and fill the smaller gaps in
    # premultiplied color. Only the source and destination cels are shared.
    original_height, original_width = poses.shape[1:3]
    width = MOTION_WIDTH
    height = round(original_height * width / original_width)
    prepared = [resize_frame(frame, width, height) for frame in poses]
    starts, directory, blocks, current = [0], [], [], []
    frame_count = 0
    with CabinetCodec(4, decode=True) as reader, CabinetCodec(5) as writer:
        def flush():
            palette, planes = indexed_frames(np.concatenate(current))
            planes = planes.reshape(len(current), width * height)
            delta = np.concatenate((planes[:1], planes[1:] ^ planes[:-1]))
            payload = palette + delta.tobytes()
            packet = writer.convert(payload, 2 * len(payload))
            blocks.append(packet)
            starts.append(starts[-1] + len(packet))
            current.clear()

        def add(frame):
            nonlocal frame_count
            current.append(frame)
            frame_count += 1
            if len(current) == FRAMES_PER_BLOCK:
                flush()

        for endpoint in prepared:
            add(endpoint)
        for a, b in sorted(edges):
            # Reactive handoffs have only a few display ticks. Give the extra
            # samples to the continuous motions and the new built-in accents.
            accent = any(96 <= key % POSES < 112 for key in (a, b))
            subdivisions = 3 if edges[a, b] > 7 or accent else 2
            count = (edges[a, b] + 1) * subdivisions - 1
            directory.append(struct.pack("<HHHH", a, b, frame_count, count))
            path = [prepared[a]]
            for packet in packets[a, b]:
                frame = unpack_frame(reader, packet, original_width, original_height)
                path.append(resize_frame(frame, width, height))
            path.append(prepared[b] if b < POSES else prepared[b - POSES][:, ::-1])
            for index, (first, second) in enumerate(zip(path, path[1:])):
                if index:
                    add(first)
                x, y = first.astype(np.uint16), second.astype(np.uint16)
                for j in range(1, subdivisions):
                    add((((subdivisions - j) * x + j * y + subdivisions // 2) // subdivisions).astype(np.uint8))
        if current:
            flush()
    header = struct.pack("<4sHHHH", b"VTM3", width, height, len(edges), frame_count)
    offsets = struct.pack(f"<{len(starts)}I", *starts)
    size = len(header) + 8 * len(edges) + len(offsets) + starts[-1]
    if size > 17_450_000:
        raise ValueError(f"motion pack is {size:,} bytes and would exceed the app's update budget")
    candidate = output.with_suffix(output.suffix + ".tmp")
    candidate.write_bytes(b"".join([header, *directory, offsets, *blocks]))
    candidate.replace(output)
    print(f"{len(edges)} transitions, {frame_count} frames, {size:,} bytes")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("poses", type=Path, help="binary output from tailmon_pose_export")
    parser.add_argument("rife", type=Path, help="RIFE NCNN Vulkan executable")
    parser.add_argument("model", type=Path, help="rife-v4.6 model directory")
    parser.add_argument("work", type=Path, help="empty or resumable scratch directory")
    parser.add_argument("output", type=Path, help="motion.bin output")
    parser.add_argument("--reuse", type=Path, help="reuse compatible frames from a VTM2 motion pack")
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    with args.poses.open("rb") as file:
        width, height = struct.unpack("<ii", file.read(8))
    poses = np.memmap(args.poses, mode="c", dtype=np.uint8, offset=8, shape=(POSES, height, width, 4))
    accent_poses(poses)
    edges = transition_edges()
    with CabinetCodec(4) as encoder:
        def encode(bgra):
            palette, plane = indexed_frames(np.ascontiguousarray(bgra[..., [2, 1, 0, 3]]))
            payload = palette + plane.tobytes()
            return encoder.convert(payload, 2 * len(payload))

        endpoints = [encode(pose) for pose in poses] if args.reuse else None
        packets = reuse_frames(args.reuse, width, height, endpoints, edges)
        print(f"Reusing {len(packets)} compatible transitions", flush=True)
        for count in sorted(set(edges.values())):
            pairs = [key for key in sorted(edges) if edges[key] == count and key not in packets]
            if not pairs:
                continue
            keys, segments = arrange(pairs)
            print(f"Baking {len(pairs)} edges with {count} intermediate frames each", flush=True)
            dark = infer(args.rife, args.model, args.work, poses, keys, count, 0)
            light = infer(args.rife, args.model, args.work, poses, keys, count, 255)
            for pair, (first, reverse) in segments.items():
                frames = []
                for i in range(1, count + 1):
                    at = first * (count + 1) + i + 1
                    path = f"{at:06d}.png"
                    frames.append(encode(recover_frame(dark / path, light / path)))
                packets[pair] = frames[::-1] if reverse else frames
    print("Packing indexed motion blocks", flush=True)
    write_pack(args.output, poses, edges, packets)


if __name__ == "__main__":
    main()
