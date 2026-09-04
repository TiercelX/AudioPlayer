"""Report anonymous JOC-slot versus RIFF ADM-object PCM correlation and tones.

This diagnostic reader deliberately rejects BW64/RF64 because it does not
resolve ds64-sized chunks.
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import tempfile
from pathlib import Path

import numpy as np
from scipy.signal import correlate, correlation_lags


def chunks(path: Path):
    with path.open("rb") as stream:
        if stream.read(4) != b"RIFF":
            raise ValueError("source must be RIFF/WAVE; BW64/RF64 is unsupported")
        stream.read(4)
        if stream.read(4) != b"WAVE":
            raise ValueError("source must be RIFF/WAVE; BW64/RF64 is unsupported")
        while True:
            header = stream.read(8)
            if not header:
                return
            if len(header) != 8:
                raise ValueError("truncated WAV chunk header")
            name = header[:4].decode("latin1")
            size = struct.unpack("<I", header[4:])[0]
            offset = stream.tell()
            yield name, size, offset
            stream.seek(size + (size & 1), 1)


def read_source(path: Path, seconds: float, object_start: int, object_count: int):
    table = {name: (size, offset) for name, size, offset in chunks(path)}
    fmt_size, fmt_offset = table["fmt "]
    with path.open("rb") as stream:
        stream.seek(fmt_offset)
        audio_format, channels, rate, _br, block, bits = struct.unpack("<HHIIHH", stream.read(fmt_size))
        if audio_format != 1 or bits != 24 or object_start + object_count > channels:
            raise ValueError("source must be PCM S24LE with enough object channels")
        data_size, data_offset = table["data"]
        frames = min(int(rate * seconds), data_size // block)
        stream.seek(data_offset)
        raw = stream.read(frames * channels * 3)
    packed = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
    values = packed[:, 0].astype(np.int32) | (packed[:, 1].astype(np.int32) << 8) | (packed[:, 2].astype(np.int32) << 16)
    values = (values ^ 0x800000) - 0x800000
    return (values.astype(np.float64) / 8388608.0).reshape(frames, channels), rate


def read_bundle(path: Path, object_count: int):
    arrays, batches, expected = [], [], 0
    for batch_path in sorted(path.glob("batch-*.bin")):
        with batch_path.open("rb") as stream:
            if stream.read(4) != b"BSCN":
                raise ValueError(f"bad bundle magic: {batch_path}")
            version, objects, samples = struct.unpack("<III", stream.read(12))
            start, end = struct.unpack("<qq", stream.read(16))
            if version != 2 or objects != object_count or start != expected or end != start + samples:
                raise ValueError(f"bad batch header: {batch_path.name}")
            audio = np.frombuffer(stream.read(objects * samples * 4), dtype="<f4")
            if audio.size != objects * samples:
                raise ValueError(f"short object PCM: {batch_path.name}")
            arrays.append(audio.reshape(objects, samples).astype(np.float64))
            batches.append({"file": batch_path.name, "start": start, "end": end})
            expected = end
    if not arrays:
        raise ValueError("empty bundle")
    return np.concatenate(arrays, axis=1), batches


def metrics(output, source, max_lag=2048):
    n = min(output.size, source.size)
    y, x = output[:n] - np.mean(output[:n]), source[:n] - np.mean(source[:n])
    sx, sy = float(np.linalg.norm(x)), float(np.linalg.norm(y))
    if sx == 0.0 or sy == 0.0:
        return {"correlation": None, "lagSamples": None, "gain": None,
                "outputRms": float(np.sqrt(np.mean(output[:n] ** 2)))}
    corr = correlate(y, x, mode="full", method="fft")
    lags = correlation_lags(n, n, mode="full")
    keep = np.abs(lags) <= max_lag
    index = np.flatnonzero(keep)[int(np.argmax(np.abs(corr[keep])))]
    lag = int(lags[index])
    if lag >= 0:
        a, b = source[:n - lag], output[lag:n]
    else:
        a, b = source[-lag:n], output[:n + lag]
    return {"correlation": float(corr[index] / (sx * sy)), "lagSamples": lag,
            "gain": float(np.dot(a, b) / max(np.dot(a, a), 1e-30)),
            "outputRms": float(np.sqrt(np.mean(output[:n] ** 2)))}


def report(source_path: Path, bundle_path: Path, object_start: int = 10,
           source_object_count: int = 8, decoded_object_count: int = 15,
           seconds: float = 5.0, frequencies: list[float] | None = None) -> dict:
    source, rate = read_source(source_path, seconds, object_start, source_object_count)
    decoded, batches = read_bundle(bundle_path, decoded_object_count)
    n = min(source.shape[0], decoded.shape[1])
    objects = source[:n, object_start:object_start + source_object_count].T
    matrix, assignments = [], []
    names = ["L", "R", "C", "Lm", "Rm", "Ls", "Rs", "Mono"]
    for index, output in enumerate(decoded[:, :n], 1):
        row = [metrics(output, candidate) for candidate in objects]
        matrix.append(row)
        best = max(range(len(row)), key=lambda i: abs(row[i]["correlation"] or 0.0))
        assignments.append({"decodedObjectIndex": index, "bestSourceTrack": object_start + best + 1,
                            "bestSourceObjectName": names[best], "best": row[best]})
    tone_matrix = None
    if frequencies:
        lo, hi = int(rate * 0.5), min(n, int(rate * 4.5))
        t = np.arange(hi - lo, dtype=np.float64) / rate
        tone_matrix = []
        for output in decoded[:, lo:hi]:
            row = []
            for index, frequency in enumerate(frequencies):
                basis = np.exp(-2j * np.pi * float(frequency) * t)
                source_coeff = 2.0 * np.dot(objects[index, lo:hi], basis) / len(t)
                output_coeff = 2.0 * np.dot(output, basis) / len(t)
                row.append({"sourceFrequencyHz": frequency,
                            "sourceMagnitude": float(abs(source_coeff)),
                            "outputMagnitude": float(abs(output_coeff)),
                            "amplitudeRatio": float(abs(output_coeff) / max(abs(source_coeff), 1e-30)),
                            "phaseDifferenceRad": float(np.angle(output_coeff / source_coeff))})
            tone_matrix.append(row)
    return {"source": str(source_path.resolve()), "bundle": str(bundle_path.resolve()),
            "sampleRate": rate, "comparedSamples": n, "decodedObjects": decoded_object_count,
            "sourceObjectTracks": list(range(object_start + 1, object_start + source_object_count + 1)),
            "batches": batches, "assignmentByMaxAbsCorrelation": assignments,
            "correlationLagGainMatrix": matrix, "frequencyResponseMatrix": tone_matrix,
            "method": "zero-mean normalized correlation, +/-2048 sample lag, anonymous slots",
            "limitations": ["Correlation is not object identity when source tracks correlate.",
                            "Renderer-neutral object PCM is not a BEAR equivalence claim."]}


def self_test() -> None:
    """Verify that the diagnostic reader does not pretend to parse BW64/RF64."""
    stream = tempfile.NamedTemporaryFile(suffix=".wav", delete=False)
    try:
        stream.write(b"BW64" + struct.pack("<I", 4) + b"WAVE")
        stream.flush()
        stream.close()
        try:
            list(chunks(Path(stream.name)))
        except ValueError as error:
            if "BW64/RF64 is unsupported" not in str(error):
                raise AssertionError(f"unexpected container rejection: {error}") from error
        else:
            raise AssertionError("BW64 unexpectedly accepted by RIFF-only reader")
    finally:
        try:
            os.unlink(stream.name)
        except FileNotFoundError:
            pass
    print("jocObjectBundleAnalyzerSelfTest=PASS cases=1 riffOnly=1")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path, nargs="?")
    parser.add_argument("bundle", type=Path, nargs="?")
    parser.add_argument("output", type=Path, nargs="?")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--seconds", type=float, default=5.0)
    parser.add_argument("--object-start", type=int, default=10)
    parser.add_argument("--frequencies", type=float, nargs=8)
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if args.source is None or args.bundle is None or args.output is None:
        parser.error("source, bundle, and output are required unless --self-test is used")
    args.output.write_text(json.dumps(report(args.source, args.bundle, args.object_start,
                                              frequencies=args.frequencies), indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
