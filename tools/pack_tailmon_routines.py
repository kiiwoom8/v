"""Append standalone Tailmon PNG animations to a VTM3 motion pack."""

import argparse
from functools import lru_cache
from pathlib import Path
import struct

import numpy as np
from PIL import Image, ImageSequence

from bake_tailmon_motion import CabinetCodec, FRAMES_PER_BLOCK, indexed_frames


ROUTINE_MARKER = 0xFFFF
BRIDGE_ROUTINE_MARKER = 0xFFFE
FRAME_MS = 33
ROUTINE_MARKERS = {33: BRIDGE_ROUTINE_MARKER, 50: ROUTINE_MARKER}
PACK_LIMIT = 20_100_000


class MotionPack:
    def __init__(self, path):
        raw = path.read_bytes()
        magic, self.width, self.height, edges, self.frames = struct.unpack_from("<4sHHHH", raw)
        if magic != b"VTM3":
            raise ValueError("the base pack must be VTM3")
        index_at = 12 + 8 * edges
        self.entries = list(struct.iter_unpack("<HHHH", raw[12:index_at]))
        blocks = (self.frames + FRAMES_PER_BLOCK - 1) // FRAMES_PER_BLOCK
        offsets = struct.unpack_from(f"<{blocks + 1}I", raw, index_at)
        payload = index_at + 4 * (blocks + 1)
        self.packets = [raw[payload + a:payload + b] for a, b in zip(offsets, offsets[1:])]
        self.area = self.width * self.height

    def unpack(self, reader, block):
        size = min(FRAMES_PER_BLOCK, self.frames - block * FRAMES_PER_BLOCK)
        return reader.convert(self.packets[block], 1024 + self.area * size)

    @lru_cache(maxsize=192)
    def frame(self, reader, index):
        block, offset = divmod(index, FRAMES_PER_BLOCK)
        raw = self.unpack(reader, block)
        colors = np.frombuffer(raw, dtype=np.uint8, count=1024).reshape(256, 4)
        delta = np.frombuffer(raw, dtype=np.uint8, offset=1024).reshape(-1, self.height, self.width)
        planes = np.bitwise_xor.reduce(delta[:offset + 1], axis=0)
        return colors[planes][..., [2, 1, 0, 3]]


def clip_frames(path, size, frame_ms=FRAME_MS):
    with Image.open(path) as image:
        if not image.is_animated or image.size != size:
            raise ValueError(f"{path} must be an animated PNG sized {size}")
        for frame in ImageSequence.Iterator(image):
            duration = frame.info.get("duration", 0)
            if not duration or duration % frame_ms:
                raise ValueError(f"{path} frame durations must be multiples of {frame_ms} ms")
            # VTM3 stores premultiplied pixels and PNG stores straight alpha.
            pixels = np.asarray(frame.convert("RGBa"))
            for _ in range(int(duration) // frame_ms):
                yield pixels


def pack_routines(base, assets, output, frame_ms=None):
    paths = sorted(assets.glob("*.png"))
    if not paths:
        raise ValueError("no animation PNGs to pack")
    if frame_ms is None:
        with Image.open(paths[0]) as image:
            for frame in ImageSequence.Iterator(image):
                duration = frame.info.get("duration", 0)
                rates = [rate for rate in ROUTINE_MARKERS if duration and duration % rate == 0]
                if len(rates) == 1:
                    frame_ms = rates[0]
                    break
        if frame_ms is None:
            raise ValueError("could not infer the frame time; use --frame-ms")
    pack = MotionPack(base)
    marker = ROUTINE_MARKERS[frame_ms]
    entries = [entry for entry in pack.entries if entry[0] not in ROUTINE_MARKERS.values()]
    frames = entries[-1][2] + entries[-1][3]
    full_blocks, remainder = divmod(frames, FRAMES_PER_BLOCK)
    packets = pack.packets[:full_blocks]
    with CabinetCodec(5) as encoder, CabinetCodec(5, decode=True) as decoder:
        def encode_block(block):
            palette, planes = indexed_frames(np.concatenate(block))
            planes = planes.reshape(len(block), pack.area)
            delta = np.concatenate((planes[:1], planes[1:] ^ planes[:-1]))
            payload = palette + delta.tobytes()
            return encoder.convert(payload, 2 * len(payload))

        if remainder:
            # Preserve the exact palette and index planes of the original
            # frames. Unused slots repeat the last frame through a zero delta.
            payload = pack.unpack(decoder, full_blocks)[:1024 + pack.area * remainder]
            payload += bytes(pack.area * (FRAMES_PER_BLOCK - remainder))
            packets.append(encoder.convert(payload, 2 * len(payload)))
            frames += FRAMES_PER_BLOCK - remainder

        block = []
        for number, path in enumerate(paths, 1):
            clip = list(clip_frames(path, (pack.width, pack.height), frame_ms))
            if len(clip) < 3 or not np.array_equal(clip[0], clip[-1]):
                raise ValueError(f"{path} must begin and end on the same neutral drawing")
            # The executable already has the neutral endpoint of each routine.
            entries.append((marker, number, frames, len(clip) - 2))
            for frame in clip[1:-1]:
                block.append(frame)
                frames += 1
                if len(block) == FRAMES_PER_BLOCK:
                    packets.append(encode_block(block))
                    block = []
        if block:
            packets.append(encode_block(block))

    offsets = [0]
    for packet in packets:
        offsets.append(offsets[-1] + len(packet))
    header = struct.pack("<4sHHHH", b"VTM3", pack.width, pack.height, len(entries), frames)
    directory = b"".join(struct.pack("<HHHH", *entry) for entry in entries)
    index = struct.pack(f"<{len(offsets)}I", *offsets)
    size = len(header) + len(directory) + len(index) + offsets[-1]
    if size > PACK_LIMIT:
        raise ValueError(f"the motion pack is {size:,} bytes and would exceed the update budget")
    candidate = output.with_suffix(output.suffix + ".tmp")
    candidate.write_bytes(b"".join([header, directory, index, *packets]))
    candidate.replace(output)
    print(f"{sum(entry[0] == marker for entry in entries)} PNG animations, {size:,} bytes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base", type=Path)
    parser.add_argument("assets", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--frame-ms", type=int, choices=ROUTINE_MARKERS)
    args = parser.parse_args()
    pack_routines(args.base, args.assets, args.output, args.frame_ms)
