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
SHEETS = dict(WALK_FIRST=16, REST_FIRST=32, COMMAND_FIRST=48, WALK_DETAIL_FIRST=64,
              ATTENTION_FIRST=80, SCROLL_DETAIL_FIRST=96, IDLE_DETAIL_FIRST=112,
              INTERACTION_FIRST=128)
TIMELINE = Path(__file__).resolve().parents[1] / "secretary.c"


def transition_edges():
    edges = {}

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

    for name, clip in re.findall(r"static const PoseStep (\w+)\[\] = \{(.*?)\};", TIMELINE.read_text(), re.S):
        poses = []
        for expression in re.findall(r"\{([^,]+),", clip):
            base, _, offset = expression.partition("+")
            base = base.strip()
            poses.append((int(base) if base.isdecimal() else SHEETS[base]) + int(offset or 0))
        run(poses, name in {"idle", "grabbed", "breathing_detail", "grab_detail", "sleeping"},
            7 if name in {"breathing_detail", "falling_asleep", "grab_detail", "grooming", "settling",
                          "sleeping", "stretching", "turning"} else 3)

    for first, length in ((16, 8), (24, 4), (64, 16), (80, 16)):
        run(list(range(first, first + length)), True, 7 if first in (64, 80) else 3)
    for first in (48, 52, 56, 60):
        run(list(range(first, first + 4)))
    run([52, 53, 54, 53, 55])
    run([80, 81, 82, 83, 84, 85, 87, 90, 93, 95])

    for idle in range(112, 120):
        for entry in (12, 32, 44, 52, 56, 60, 80, 120, 128, 136):
            edge(idle, entry, 7 if entry == 128 else 3)
        edge(idle, 31, 7, True)
        edge(idle, 31, 7)
    for exit_pose in (0, 4, 12, 15, 43, 47, 51, 55, 59, 63, 95, 127, 143):
        edge(exit_pose, 112, 7 if exit_pose in (43, 47, 143) else 3)
    edge(31, 112, 7, True)
    for phase in range(64, 80):
        edge(phase, 28, 7)
        edge(phase, 80, 3, True)
        edge(phase, 128, 3, True)
    for phase in (16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27):
        edge(phase, 28, 3)
    for phase in (16, 24, 64):
        edge(phase, 28, 7)
    edge(35, 36, 7)
    for phase in range(32, 36):
        edge(phase, 128, 2)
    for phase in range(36, 40):
        edge(phase, 40, 7)
        edge(phase, 128, 7)
    for a, b in ((29, 30), (30, 31), (38, 40), (40, 41), (40, 43)):
        edge(a, b, 15)
    edge(53, 55, 7)
    for phase in range(128, 136):
        edge(phase, 142, 15 if phase in (128, 131, 132, 135) else 7)
        edge(phase, 136, 3)
    for first, last in ((28, 31), (40, 47), (80, 95), (120, 127), (136, 143)):
        for phase in range(first, last + 1):
            edge(phase, 128, 3, first == 28)
    for phase in (28, 29, 30, 31):
        edge(phase, 80, 3, True)
        edge(phase, 112, 2, True)
    for phase in range(80, 96):
        edge(phase, 112)
    for phase in (*range(40, 47), *range(120, 127)):
        edge(phase, 80, 2)
    for last in (15, 43, 47, 51, 55, 59, 63, 95, 127, 143):
        for entry in (12, 52, 56, 60, 80, 136):
            edge(last, entry)
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


class FrameEncoder:
    """An indexed premultiplied BGRA frame in a Windows XPRESS Huffman buffer."""

    def __init__(self):
        lib = ct.WinDLL("cabinet.dll", use_last_error=True)
        create = lib.CreateCompressor
        create.argtypes = [ct.c_ulong, ct.c_void_p, ct.POINTER(ct.c_void_p)]
        self.compress = lib.Compress
        self.compress.argtypes = [ct.c_void_p, ct.c_void_p, ct.c_size_t, ct.c_void_p,
                                  ct.c_size_t, ct.POINTER(ct.c_size_t)]
        self.close = lib.CloseCompressor
        self.close.argtypes = [ct.c_void_p]
        self.handle = ct.c_void_p()
        if not create(4, None, ct.byref(self.handle)):
            raise ct.WinError(ct.get_last_error())

    def encode(self, bgra):
        rgba = np.ascontiguousarray(bgra[..., [2, 1, 0, 3]])
        indexed = Image.fromarray(rgba).quantize(colors=256, method=Image.Quantize.FASTOCTREE)
        palette = bytearray(1024)
        for index, (r, g, b, a) in enumerate(np.frombuffer(indexed.palette.palette, dtype=np.uint8).reshape(-1, 4)):
            slot = index * 4
            palette[slot:slot + 4] = bytes((min(b, a), min(g, a), min(r, a), a))
        payload = bytes(palette) + indexed.tobytes()
        output = ct.create_string_buffer(2 * len(payload))
        length = ct.c_size_t()
        if not self.compress(self.handle, payload, len(payload), output, len(output), ct.byref(length)):
            raise ct.WinError(ct.get_last_error())
        return output.raw[:length.value]


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("poses", type=Path, help="binary output from tailmon_pose_export")
    parser.add_argument("rife", type=Path, help="RIFE NCNN Vulkan executable")
    parser.add_argument("model", type=Path, help="rife-v4.6 model directory")
    parser.add_argument("work", type=Path, help="empty or resumable scratch directory")
    parser.add_argument("output", type=Path, help="motion.bin output")
    parser.add_argument("--reuse", type=Path, help="reuse compatible frames from an earlier motion pack")
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    with args.poses.open("rb") as file:
        width, height = struct.unpack("<ii", file.read(8))
    poses = np.memmap(args.poses, mode="r", dtype=np.uint8, offset=8, shape=(POSES, height, width, 4))
    edges = transition_edges()
    encoder = FrameEncoder()
    try:
        endpoints = [encoder.encode(pose) for pose in poses]
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
                    frames.append(encoder.encode(recover_frame(dark / path, light / path)))
                packets[pair] = frames[::-1] if reverse else frames
    finally:
        encoder.close(encoder.handle)

    frame_count = POSES + sum(edges.values())
    starts, directory, frames = [], [], list(endpoints)
    for key in sorted(edges):
        directory.append(struct.pack("<HHHH", *key, len(frames), edges[key]))
        frames.extend(packets[key])
    cursor = 0
    for frame in frames:
        starts.append(struct.pack("<I", cursor))
        cursor += len(frame)
    starts.append(struct.pack("<I", cursor))
    header = struct.pack("<4sHHHH", b"VTM2", width, height, len(edges), frame_count)
    size = len(header) + 8 * len(edges) + 4 * (frame_count + 1) + cursor
    if size > 17_450_000:
        raise ValueError(f"motion pack is {size:,} bytes and would exceed the app's update budget")
    args.output.write_bytes(b"".join([header, *directory, *starts, *frames]))
    print(f"{len(edges)} transitions, {frame_count - POSES} new frames, {size:,} bytes")


if __name__ == "__main__":
    main()
