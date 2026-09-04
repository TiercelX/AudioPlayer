"""Compare pinned BEAR binaural impulses with the project's BBC System-H cache.

This is a diagnostic boundary comparison; it does not claim sample equality.
"""

import argparse
import json
import os
import struct
import subprocess
import sys
import math
from pathlib import Path

import numpy as np


PROJECT_XYZ = {
    "front": (1.0, 0.0, 0.0),
    "left": (0.0, 1.0, 0.0),
    "right": (0.0, -1.0, 0.0),
    "rear": (-1.0, 0.0, 0.0),
    "upper": (0.0, 0.0, 1.0),
    "lower": (0.0, 0.0, -1.0),
    "interior": (0.7071067811865475, 0.5, 0.5),
}
POSITIONS = {n: (math.degrees(math.atan2(y, x)), math.degrees(math.asin(z)))
             for n, (x, y, z) in PROJECT_XYZ.items()}


def metrics(x, sample_rate=48000):
    x = np.asarray(x, dtype=np.float64)
    mag = np.max(np.abs(x), axis=0)
    peak = float(np.max(mag))
    onset = int(np.flatnonzero(mag >= peak * 10 ** (-60 / 20))[0]) if peak else None
    tail = np.flatnonzero(mag >= peak * 10 ** (-80 / 20)) if peak else np.array([])
    direct_end = min((onset or 0) + 256, x.shape[1])
    late_start = min((onset or 0) + 1024, x.shape[1])
    return {
        "finite": bool(np.isfinite(x).all()),
        "nonzero": bool(np.count_nonzero(x)),
        "frames": int(x.shape[1]),
        "energy": float(np.sum(x * x)),
        "peakPerEar": [float(np.max(np.abs(x[e]))) for e in range(2)],
        "energyPerEar": [float(np.sum(x[e] * x[e])) for e in range(2)],
        "onsetSampleRelativeMinus60dB": onset,
        "tailStopSampleAtMinus80dB": int(tail[-1]) if len(tail) else None,
        "tailThreshold": "peak-relative -80 dB",
        "tailTruncated": bool(len(tail) and tail[-1] == x.shape[1] - 1),
        "tailDurationMsAtMinus80dB": (1000.0 * float(tail[-1] - (onset or 0) + 1) / sample_rate) if len(tail) else 0.0,
        "directEnergyOnsetPlus256": float(np.sum(x[:, (onset or 0):direct_end] ** 2)),
        "lateEnergyOnsetPlus1024": float(np.sum(x[:, late_start:] ** 2)),
        "lateToDirectRatio": float(np.sum(x[:, late_start:] ** 2) / max(np.sum(x[:, (onset or 0):direct_end] ** 2), 1e-30)),
        "lateToTotalRatio": float(np.sum(x[:, late_start:] ** 2) / max(np.sum(x * x), 1e-30)),
        "lrEnergyRatio": float(np.sum(x[0] * x[0]) / max(np.sum(x[1] * x[1]), 1e-30)),
    }


def parse_vectors(executable):
    text = subprocess.check_output([str(executable), "--vectors"], text=True)
    result = {}
    for line in text.splitlines():
        words = line.split()
        if len(words) < 26 or words[0] != "VECTOR":
            continue
        if words[2] != "Selected":
            raise RuntimeError(f"unexpected panner status: {line}")
        name = words[1]
        power = float(words[3].split("=", 1)[1])
        gains = np.array([float(v) for v in words[4:26]], dtype=np.float64)
        if len(gains) != 22 or not np.isfinite(gains).all():
            raise RuntimeError(f"invalid project vector: {line}")
        result[name] = (power, gains)
    if set(result) != set(POSITIONS):
        raise RuntimeError(f"missing project vectors: {set(POSITIONS) ^ set(result)}")
    return result


def load_cache(path):
    raw = path.read_bytes()
    fields = struct.unpack_from("<8s6IiI", raw, 0)
    magic, version, header_size, sample_rate, emitters, receivers, ir_length, listener, hash_len = fields
    if magic != b"R2A1BRIR" or version != 1 or header_size != 520 or sample_rate != 48000 or emitters != 22 or receivers != 2 or listener != 0 or hash_len != 32:
        raise RuntimeError("unexpected System-H cache header")
    if ir_length != 16384 or len(raw) != 520 + 2 * 22 * ir_length * 4:
        raise RuntimeError("unexpected System-H cache size")
    mapping = struct.unpack_from("<22I", raw, 72)
    left, right = struct.unpack_from("<2I", raw, 160)
    delays = np.frombuffer(raw, dtype="<f8", count=44, offset=168).reshape(2, 22)
    if sorted(mapping) != list(range(22)) or left >= 2 or right >= 2 or left == right:
        raise RuntimeError("invalid System-H cache mapping")
    if not np.isfinite(delays).all() or np.any(delays != 0):
        raise RuntimeError("current BrirConvolver rejects nonzero/fractional cache delays")
    ir = np.frombuffer(raw, dtype="<f4", offset=520).reshape(2, 22, ir_length)
    if not np.isfinite(ir).all():
        raise RuntimeError("non-finite System-H cache")
    return sample_rate, ir, {"mapping": list(mapping), "leftReceiverIndex": left, "rightReceiverIndex": right,
                             "delayPolicy": "exact current BrirConvolver policy: all delays must be finite zero; no integer/fractional delay applied",
                             "delays": delays.tolist()}


def bear_impulses(args):
    for p in [args.bear_python, args.visr_python, args.bear_dll, args.visr_dll, args.boost_dll, args.data]:
        if not p.exists():
            raise RuntimeError(f"missing explicit BEAR path: {p}")
    for p in [args.bear_dll, args.visr_dll, args.boost_dll]:
        os.add_dll_directory(str(p.resolve()))
    sys.path[:0] = [str(args.bear_python.resolve()), str(args.visr_python.resolve())]
    import visr_bear

    out = {}
    for name, (azimuth, elevation) in POSITIONS.items():
        c = visr_bear.api.Config()
        c.num_objects_channels = 1
        c.num_direct_speakers_channels = 1
        c.num_hoa_channels = 1
        c.period_size = 512
        c.data_path = str(args.data.resolve())
        renderer = visr_bear.api.Renderer(c)
        obj_meta = visr_bear.api.ObjectsInput()
        obj_meta.rtime = visr_bear.api.Time(0, 1)
        obj_meta.duration = visr_bear.api.Time(1, 1)
        obj_meta.type_metadata.position = visr_bear.api.PolarPosition(azimuth, elevation, 1)
        renderer.add_objects_block(0, obj_meta)
        audio = np.zeros((2, 16384), dtype=np.float32)
        for frame in range(32):
            obj = np.zeros((1, 512), dtype=np.float32)
            if frame == 0:
                obj[0, 0] = 1.0
            silence = np.ascontiguousarray(np.zeros((1, 512), dtype=np.float32))
            renderer.process(obj, silence, silence, audio[:, frame * 512 : (frame + 1) * 512])
        out[name] = audio
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bear-python", type=Path, required=True)
    ap.add_argument("--visr-python", type=Path, required=True)
    ap.add_argument("--bear-dll", type=Path, required=True)
    ap.add_argument("--visr-dll", type=Path, required=True)
    ap.add_argument("--boost-dll", type=Path, required=True)
    ap.add_argument("--data", type=Path, required=True)
    ap.add_argument("--project-panner", type=Path, required=True)
    ap.add_argument("--project-cache", type=Path, required=True)
    ap.add_argument("--report", type=Path, required=True)
    args = ap.parse_args()
    project = parse_vectors(args.project_panner)
    sample_rate, ir, cache_meta = load_cache(args.project_cache)
    bear = bear_impulses(args)
    cases = {}
    for name in POSITIONS:
        _, gains = project[name]
        project_audio = np.zeros((2, ir.shape[2]), dtype=np.float64)
        project_audio = np.einsum("s,est->et", gains, ir, optimize=True)
        xyz = PROJECT_XYZ[name]
        if abs(sum(v * v for v in xyz) - 1.0) > 1e-12:
            raise RuntimeError(f"non-unit project vector: {name}")
        cases[name] = {"projectXyz": list(xyz), "coordinates": {"azimuthDegrees": POSITIONS[name][0], "elevationDegrees": POSITIONS[name][1]},
                       "projectSystemH": metrics(project_audio, sample_rate),
                       "bear": metrics(bear[name], sample_rate)}
    report = {"result": "PASS", "sampleRate": sample_rate, "positions": cases,
              "tailPolicy": "32 x 512-frame periods (16384 frames); onset is peak-relative -60 dB; tail is peak-relative -80 dB; direct is onset..onset+255 and late is onset+1024..end; tailTruncated is explicit",
              "coordinateConvention": "canonical project xyz [front,left,up], unit norm; BEAR PolarPosition derives azimuth=atan2(left,front), elevation=asin(up)",
              "speakerOrder": "project checked-in BS.2051 System-H 22-channel order; cache payload reordered to that order",
              "cacheValidation": cache_meta}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"result": report["result"], "positions": list(cases)}))


if __name__ == "__main__":
    main()
