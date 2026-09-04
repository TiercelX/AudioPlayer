"""Measure the finite peak of an interleaved little-endian f32 stream."""
import argparse
import json
from pathlib import Path

import numpy as np


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--channels", type=int, required=True)
    args = parser.parse_args()
    if args.channels <= 0:
        raise ValueError("channels must be positive")
    size = args.input.stat().st_size
    frame_bytes = args.channels * 4
    if size == 0 or size % frame_bytes:
        raise ValueError("input is not a non-empty whole-frame f32 stream")
    peak = 0.0
    frames = 0
    with args.input.open("rb") as handle:
        while True:
            raw = handle.read(1024 * 1024)
            if not raw:
                break
            if len(raw) % frame_bytes:
                raise ValueError("truncated f32 frame")
            samples = np.frombuffer(raw, dtype="<f4")
            if not np.isfinite(samples).all():
                raise ValueError("nonfinite f32 sample")
            peak = max(peak, float(np.max(np.abs(samples))))
            frames += len(samples) // args.channels
    peak_dbfs = 20.0 * np.log10(max(peak, 1.0e-15))
    # Keep at least 1 dBFS headroom while retaining the requested -2 dB
    # audition attenuation whenever that is already sufficient.
    effective_gain_db = min(-2.0, -1.0 - peak_dbfs)
    print(json.dumps({
        "frames": frames,
        "channels": args.channels,
        "peak": peak,
        "peakDbfs": peak_dbfs,
        "requestedGainDb": -2.0,
        "effectiveGainDb": effective_gain_db,
        "postGainPeakDbfs": peak_dbfs + effective_gain_db,
        "headroomDb": -(peak_dbfs + effective_gain_db),
    }, indent=2))


if __name__ == "__main__":
    main()
