"""Wrap little-endian interleaved IEEE-f32 samples in a RIFF/WAVE header."""
import argparse
import struct
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--channels", type=int, required=True)
    parser.add_argument("--rate", type=int, required=True)
    args = parser.parse_args()
    if args.channels <= 0 or args.rate <= 0:
        raise ValueError("channels and rate must be positive")
    size = args.input.stat().st_size
    frame_bytes = args.channels * 4
    if size == 0 or size % frame_bytes:
        raise ValueError("input is not a non-empty whole-frame f32 stream")
    fmt = struct.pack("<HHIIHH", 3, args.channels, args.rate,
                      args.rate * frame_bytes, frame_bytes, 32)
    with args.input.open("rb") as source, args.output.open("wb") as target:
        target.write(b"RIFF" + struct.pack("<I", 36 + size) + b"WAVE")
        target.write(b"fmt " + struct.pack("<I", len(fmt)) + fmt)
        target.write(b"data" + struct.pack("<I", size))
        for block in iter(lambda: source.read(1024 * 1024), b""):
            target.write(block)


if __name__ == "__main__":
    main()
