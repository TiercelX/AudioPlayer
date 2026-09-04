"""Fast self-test for the reusable ADM/JOC oracle helpers (no media needed)."""

from __future__ import annotations

import importlib.util
import tempfile
from pathlib import Path

import numpy as np


def load(name: str):
    path = Path(__file__).with_name(name)
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def main() -> None:
    carrier = load("make_adm_diagnostic_carrier.py")
    analyzer = load("analyze_joc_object_bundle.py")
    signals = carrier.make_signal(48000, 48000, [401, 503, 607, 709, 811, 919, 1021, 1129])
    assert signals.shape == (48000, 8)
    assert np.isfinite(signals).all() and float(np.max(np.abs(signals))) < 0.06
    source = np.sin(np.arange(48000, dtype=np.float64) * 2.0 * np.pi * 401.0 / 48000.0)
    delayed = np.concatenate((np.zeros(37), source[:-37]))
    result = analyzer.metrics(delayed, source, max_lag=128)
    assert abs(result["correlation"]) > 0.99 and result["lagSamples"] == 37
    with tempfile.TemporaryDirectory() as directory:
        bw64 = Path(directory) / "unsupported-bw64.wav"
        bw64.write_bytes(b"BW64" + b"\x00" * 40)
        try:
            list(analyzer.chunks(bw64))
        except ValueError as error:
            assert "BW64/RF64 is unsupported" in str(error)
        else:
            raise AssertionError("BW64 input was not rejected")
    print("admJocOracleSelfTest=PASS cases=3")


if __name__ == "__main__":
    main()
