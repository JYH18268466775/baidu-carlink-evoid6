"""Validate an offline mdev replay trace against the original H.264 bytes."""

import argparse
import base64
import hashlib
import json
import re
import struct
import tempfile
from pathlib import Path
from shutil import copyfileobj

ANNEX_B = re.compile(rb"\x00\x00(?:\x00)?\x01(.)", re.DOTALL)
SETUP_TYPES = (5, 2, 4)
WIRE_MAGIC = b"MDEVv1\r\n"


def nal_types(payload):
    return [match[1][0] & 31 for match in ANNEX_B.finditer(payload)]


def read_message(line):
    row = json.loads(line)
    header, *body = [base64.b64decode(part, validate=True) for part in row["iov"]]
    if len(header) != 24 or header[:16] != bytes(16):
        raise ValueError("invalid 24-byte header")
    kind, length = struct.unpack_from("<II", header, 16)
    if kind != row["type"] or length != sum(map(len, body)):
        raise ValueError("header type or body length mismatch")
    return row, kind, header, body


def verify(trace, h264, packed=None):
    digest = hashlib.sha256()
    frames = 0
    fps = 0
    if packed is not None:
        packed.write(WIRE_MAGIC)
    for line_number, line in enumerate(trace, start=1):
        try:
            row, kind, header, body = read_message(line)
            expected = SETUP_TYPES[line_number - 1] if line_number <= len(SETUP_TYPES) else 3
            if kind != expected or len(body) != (2 if kind == 3 else 1):
                raise ValueError("unexpected message order or segment count")
            if kind == 2:
                codec, fourcc, width, height, depth, fps, zero_a, zero_b = struct.unpack("<8I", body[0])
                if (codec, fourcc, depth, zero_a, zero_b) != (2, 0x48323634, 16, 0, 0):
                    raise ValueError("unexpected format fields")
                if not width or not height or not 1 <= fps <= 1_000_000:
                    raise ValueError("invalid dimensions or replay rate")
            elif kind == 5:
                if body[0] != b"\x01\x00\x00\x00":
                    raise ValueError("unexpected sync payload")
            else:
                payload = body[-1]
                if not payload:
                    raise ValueError("empty video or codec configuration payload")
                if kind == 4 and nal_types(payload) != [7, 8]:
                    raise ValueError("expected SPS followed by PPS")
                if kind == 3:
                    timestamp, = struct.unpack("<Q", body[0])
                    if timestamp != frames * 1_000_000 // fps:
                        raise ValueError("unexpected synthetic replay timestamp")
                    if row["frame"] != frames or row["timestamp_us"] != timestamp:
                        raise ValueError("frame metadata disagrees with its IOV")
                    if not frames and nal_types(payload)[:1] != [5]:
                        raise ValueError("first frame does not start with an IDR")
                    frames += 1
                if h264.read(len(payload)) != payload:
                    raise ValueError("payload differs from the original H.264")
                digest.update(payload)
            if packed is not None:
                packed.write(header)
                packed.writelines(body)
        except (KeyError, TypeError, ValueError, struct.error) as error:
            raise ValueError(f"line {line_number}: {error}") from error
    if not frames or h264.read(1):
        raise ValueError("trace does not cover the complete H.264 stream")
    return frames, width, height, fps, digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("h264", type=Path)
    parser.add_argument("--pack", type=Path, help="Write a new verified binary replay file")
    args = parser.parse_args()
    try:
        with args.trace.open(encoding="utf-8") as trace, args.h264.open("rb") as h264, tempfile.TemporaryFile() as packed:
            frames, width, height, fps, digest = verify(trace, h264, packed if args.pack else None)
            if args.pack:
                packed.seek(0)
                with args.pack.open("xb") as output:
                    copyfileobj(packed, output)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")
    print(f"Verified {frames} frames, {width}x{height}, {fps} fps replay")
    print(f"H.264 SHA-256: {digest}")


if __name__ == "__main__":
    main()
