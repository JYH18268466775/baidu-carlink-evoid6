"""Build an offline mdev IOV trace from the captured CarLife video channel.

Trace lines are JSON objects with base64-encoded IOV segments. The trace is
for inspection and replay tooling. It does not open or send to a QNX device.
"""

import argparse
import base64
import json
import re
import struct
import tempfile
from pathlib import Path
from shutil import copyfileobj

from carlife_extract_video import VIDEO_DATA, VIDEO_HEADER, VIDEO_HEARTBEAT, records

START_CODE = re.compile(rb"\x00\x00(?:\x00)?\x01(?=[\x01-\xff])")
MDEV_FORMAT = 2
MDEV_FRAME = 3
MDEV_CONFIG = 4
MDEV_SYNC = 5


def mdev_header(message_type, body_length):
    # nbt-aad sendMdevFormat, sendMdevConfig, sendMdevSync and workerLoop.
    return struct.pack("<16xII", message_type, body_length)


def emit(output, kind, *parts, **metadata):
    header = mdev_header(kind, sum(map(len, parts)))
    line = {
        "type": kind,
        "iov": [base64.b64encode(part).decode("ascii") for part in (header, *parts)],
        **metadata,
    }
    output.write(json.dumps(line, separators=(",", ":")) + "\n")


def build_trace(source, output, width, height, fps):
    # Cold codec-config path in nbt-aad: Sync (0x127d50), Format (0x127ef0),
    # Codec Config (0x127f14). A configured context then accepts frame messages.
    emit(output, MDEV_SYNC, struct.pack("<I", 1))
    # The FourCC is the numeric value 0x48323634 stored in a little-endian word.
    emit(output, MDEV_FORMAT, struct.pack("<6I8x", 2, 0x48323634, width, height, 16, fps))
    frames = 0
    for (_, carlife_timestamp, kind), payload in records(source, VIDEO_HEADER, 0, "Video"):
        if kind == VIDEO_HEARTBEAT:
            continue
        if kind != VIDEO_DATA:
            raise ValueError(f"Unexpected video message type 0x{kind:08x}")
        nals = list(START_CODE.finditer(payload))
        if not nals or nals[0].start() != 0:
            raise ValueError("Expected an Annex B payload starting at a NAL boundary")
        if frames == 0:
            if [payload[nal.end()] & 31 for nal in nals[:3]] != [7, 8, 5]:
                raise ValueError("Expected the captured stream to start with SPS, PPS, and IDR")
            idr_offset = nals[2].start()
            emit(output, MDEV_CONFIG, payload[:idr_offset])
            payload = payload[idr_offset:]
        timestamp_us = frames * 1_000_000 // fps
        emit(output, MDEV_FRAME, struct.pack("<Q", timestamp_us), payload,
             frame=frames, timestamp_us=timestamp_us, carlife_timestamp=carlife_timestamp)
        frames += 1
    if not frames:
        raise ValueError("No video frames")
    return frames


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="Raw, demultiplexed CarLife video capture")
    parser.add_argument("output", type=Path, help="New JSON Lines IOV trace")
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--fps", type=int, default=30)
    args = parser.parse_args()
    if any(not 1 <= n <= 0xFFFFFFFF for n in (args.width, args.height, args.fps)):
        parser.error("width, height, and fps must be positive 32-bit integers")
    try:
        with args.input.open("rb") as source, tempfile.TemporaryFile(mode="w+", encoding="utf-8") as trace:
            frames = build_trace(source, trace, args.width, args.height, args.fps)
            trace.seek(0)
            with args.output.open("x", encoding="utf-8", newline="\n") as output:
                copyfileobj(trace, output)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")
    print(f"Wrote {frames} frames and 3 setup messages to {args.output}")


if __name__ == "__main__":
    main()
