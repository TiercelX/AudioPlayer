"""Run one official pinned visr_bear front-centre impulse probe.

All dependency paths are explicit so this does not use user site-packages.
"""

import argparse
import json
import os
import sys
from pathlib import Path

import numpy as np


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bear-python", type=Path, required=True)
    parser.add_argument("--visr-python", type=Path, required=True)
    parser.add_argument("--bear-dll", type=Path, required=True)
    parser.add_argument("--visr-dll", type=Path, required=True)
    parser.add_argument("--boost-dll", type=Path, required=True)
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()

    dll_dirs = [args.bear_dll, args.visr_dll, args.boost_dll]
    for path in [*dll_dirs, args.bear_python, args.visr_python, args.data]:
        if not path.exists():
            raise SystemExit(f"missing explicit probe path: {path}")
    for path in dll_dirs:
        os.add_dll_directory(str(path.resolve()))
    sys.path[:0] = [str(args.bear_python.resolve()), str(args.visr_python.resolve())]

    import visr_bear  # pylint: disable=import-outside-toplevel

    config = visr_bear.api.Config()
    config.num_objects_channels = 1
    config.num_direct_speakers_channels = 1
    config.num_hoa_channels = 1
    config.period_size = 512
    config.data_path = str(args.data.resolve())
    renderer = visr_bear.api.Renderer(config)
    metadata = visr_bear.api.ObjectsInput()
    metadata.rtime = visr_bear.api.Time(0, 1)
    metadata.duration = visr_bear.api.Time(1, 1)
    metadata.type_metadata.position = visr_bear.api.PolarPosition(0, 0, 1)
    renderer.add_objects_block(0, metadata)

    objects = np.zeros((1, 512), dtype=np.float32)
    objects[0, 0] = 1.0
    silence = np.ascontiguousarray(np.zeros((1, 512), dtype=np.float32))
    output = np.zeros((2, 512), dtype=np.float32)
    renderer.process(objects, silence, silence, output)
    if output.shape != (2, 512):
        raise SystemExit(f"unexpected BEAR output shape: {output.shape}")
    if not np.isfinite(output).all() or not np.count_nonzero(output):
        raise SystemExit("BEAR output failed finite/nonzero checks")

    report = {
        "result": "PASS",
        "renderer": "official visr_bear",
        "input": "front-center object impulse",
        "shape": list(output.shape),
        "finite": True,
        "peakPerEar": [float(np.max(np.abs(output[i]))) for i in range(2)],
        "nonzeroSamples": int(np.count_nonzero(output)),
        "sumSquares": float(np.sum(output * output)),
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
