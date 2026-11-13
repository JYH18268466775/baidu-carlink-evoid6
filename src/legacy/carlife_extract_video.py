"""Extract plaintext H.264 from a legacy CarLife video or AOA bulk-IN stream."""

import argparse
import shutil
import struct
import tempfile
from pathlib import Path

AOA_HEADER = struct.Struct(">II")  # channel, payload bytes
VIDEO_HEADER = struct.Struct(">III")  # payload bytes, raw timestamp, message type
VIDEO_CHANNEL = 2
VIDEO_DATA = 0x00020001
VIDEO_HEARTBEAT = 0x00020002
MAX_PAYLOAD = 64 * 1024 * 1024


def records(source, header, length_index, label):
    while data := source.read(header.size):
        offset = source.tell() - len(data)
        if len(data) != header.size:
            raise ValueError(f"{label} header truncated at byte {offset}")
        fields = header.unpack(data)
        length = fields[length_index]
        if length > MAX_PAYLOAD:
            raise ValueError(f"{label} payload too large at byte {offset}: {length}")
        payload = source.read(length)
        if len(payload) != length:
            raise ValueError(f"{label} payload truncated at byte {offset}")
        yield fields, payload


def demux_aoa(source, video):
    for (channel, _), payload in records(source, AOA_HEADER, 1, "AOA"):
        if channel not in range(1, 7):
            raise ValueError(f"Unknown AOA channel: {channel}")
        if channel == VIDEO_CHANNEL:
            video.write(payload)
    video.seek(0)


def extract_video(source, output):
    messages = 0
    for (_, _timestamp, kind), payload in records(source, VIDEO_HEADER, 0, "Video"):
        if kind == VIDEO_HEARTBEAT:
            continue
        if kind != VIDEO_DATA or not payload:
            raise ValueError(f"Unexpected video message 0x{kind:08x} ({len(payload)} bytes)")
        output.write(payload)
        messages += 1
    if not messages:
        raise ValueError("No video data messages")
    output.seek(0)
    if not output.read(4).startswith((b"\x00\x00\x01", b"\x00\x00\x00\x01")):
        raise ValueError("Video does not start with an Annex B start code; check framing or encryption")
    output.seek(0)
    return messages


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="Raw bytes with no USB capture container")
    parser.add_argument("output", type=Path, help="New Annex B .h264 file")
    parser.add_argument("--input-format", choices=("aoa", "video"), default="aoa",
                        help="Multiplexed AOA stream or already demultiplexed video channel")
    args = parser.parse_args()
    try:
        with args.input.open("rb") as source, tempfile.TemporaryFile() as result:
            if args.input_format == "aoa":
                with tempfile.TemporaryFile() as video:
                    demux_aoa(source, video)
                    count = extract_video(video, result)
            else:
                count = extract_video(source, result)
            with args.output.open("xb") as output:
                shutil.copyfileobj(result, output)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")
    print(f"Extracted {count} video messages to {args.output}")


if __name__ == "__main__":
    main()
