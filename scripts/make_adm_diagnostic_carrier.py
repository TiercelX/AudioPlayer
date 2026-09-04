"""Create a deterministic, local RIFF ADM BWF diagnostic derivative.

Only the requested initial PCM window is replaced.  All RIFF container/ADM bytes
remain copied from the source; the source is never modified.  This tool is
diagnostic, supports RIFF/WAVE only (not BW64/RF64), and does not create
Dolby-authored provenance.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import struct
from pathlib import Path

import numpy as np


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def wav_chunks(path: Path):
    with path.open("rb") as stream:
        if stream.read(4) != b"RIFF":
            raise ValueError("source is not RIFF")
        stream.read(4)
        if stream.read(4) != b"WAVE":
            raise ValueError("source is not WAVE")
        while True:
            header = stream.read(8)
            if not header:
                return
            if len(header) != 8:
                raise ValueError("truncated chunk header")
            name = header[:4].decode("latin1")
            size = struct.unpack("<I", header[4:])[0]
            offset = stream.tell()
            yield name, size, offset
            stream.seek(size + (size & 1), 1)


def make_signal(frames: int, rate: int, frequencies: list[float]) -> np.ndarray:
    t = np.arange(frames, dtype=np.float64) / rate
    gate = np.zeros(frames, dtype=np.float64)
    gate[(t >= 0.25) & (t < 4.75)] = 1.0
    fade = 0.02
    left = (t >= 0.25) & (t < 0.25 + fade)
    right = (t >= 4.75 - fade) & (t < 4.75)
    gate[left] = 0.5 - 0.5 * np.cos(np.pi * (t[left] - 0.25) / fade)
    gate[right] = 0.5 + 0.5 * np.cos(np.pi * (t[right] - (4.75 - fade)) / fade)
    objects = np.empty((frames, len(frequencies)), dtype=np.float64)
    phase = np.arange(len(frequencies), dtype=np.float64) * (np.pi / 7.0)
    for index, frequency in enumerate(frequencies):
        objects[:, index] = 0.04 * np.sin(2 * np.pi * frequency * t + phase[index]) * gate
        pulse_at = 0.10 + index * 0.015
        pulse = np.exp(-0.5 * ((t - pulse_at) / 0.0012) ** 2)
        objects[:, index] += 0.015 * pulse * np.sin(2 * np.pi * frequency * t + phase[index])
    if float(np.max(np.abs(objects))) >= 0.06:
        raise ValueError("diagnostic signal peak guard failed")
    return objects


def create(source: Path, output: Path, manifest: Path, seconds: float = 5.0,
           frequencies: list[float] | None = None) -> dict:
    source = source.resolve()
    output = output.resolve()
    if source == output:
        raise ValueError("refusing in-place modification")
    frequencies = frequencies or [401, 503, 607, 709, 811, 919, 1021, 1129]
    chunks = {name: (size, offset) for name, size, offset in wav_chunks(source)}
    fmt_size, fmt_offset = chunks["fmt "]
    with source.open("rb") as stream:
        stream.seek(fmt_offset)
        audio_format, channels, rate, _byte_rate, block_align, bits = struct.unpack(
            "<HHIIHH", stream.read(fmt_size)
        )
    if (audio_format, channels, rate, block_align, bits) != (1, 18, 48000, 54, 24):
        raise ValueError("expected 18-channel PCM S24LE at 48 kHz")
    data_size, data_offset = chunks["data"]
    frames = min(int(rate * seconds), data_size // block_align)
    if frames <= 0 or len(frequencies) != 8:
        raise ValueError("expected eight object frequencies and a non-empty window")
    shutil.copy2(source, output)
    objects = make_signal(frames, rate, frequencies)
    with output.open("r+b") as stream:
        for start in range(0, frames, 12000):
            count = min(12000, frames - start)
            interleaved = np.zeros((count, channels), dtype=np.float64)
            interleaved[:, 10:18] = objects[start:start + count]
            quantized = np.rint(np.clip(interleaved, -1.0, 0.9999998808) * 8388607.0).astype(np.int32)
            raw = np.empty((count, channels, 3), dtype=np.uint8)
            raw[:, :, 0] = quantized & 0xff
            raw[:, :, 1] = (quantized >> 8) & 0xff
            raw[:, :, 2] = (quantized >> 16) & 0xff
            stream.seek(data_offset + start * block_align)
            stream.write(raw.tobytes())
    result = {
        "source": str(source), "sourceSha256": sha256(source),
        "output": str(output), "outputSha256": sha256(output),
        "derivedDiagnosticOnly": True, "sourceWasModified": False,
        "modifiedFrameRange": [0, frames], "sampleRate": rate, "channels": channels,
        "bedChannelsZeroBased": list(range(10)), "objectChannelsOneBased": list(range(11, 19)),
        "objectFrequenciesHz": frequencies, "objectAmplitude": [0.04] * 8,
        "pulseTimesSeconds": [0.10 + i * 0.015 for i in range(8)],
        "peakGuard": float(np.max(np.abs(objects)),),
        "preservedChunks": ["JUNK", "fmt ", "axml", "chna", "dbmd"],
        "note": "ADM XML/CHNA/DBMD retained; not Dolby-authored provenance.",
    }
    manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest.write_text(json.dumps(result, indent=2), encoding="utf-8")
    return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--seconds", type=float, default=5.0)
    parser.add_argument("--frequencies", type=float, nargs=8)
    args = parser.parse_args()
    create(args.source, args.output, args.manifest, args.seconds, args.frequencies)


if __name__ == "__main__":
    main()
