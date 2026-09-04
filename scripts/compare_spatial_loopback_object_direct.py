"""Compare a bounded Spatial loopback segment with Object Direct PCM.

The comparison aligns the loopback's first detected active frame to frame zero
of the renderer-neutral reference and RMS-normalizes only for waveform shape.
It is not a spatial or listening-quality oracle.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

import numpy as np


def _read_wav(path: Path):
    source = Path(__file__).resolve().parents[1] / "tools" / "atmos-render" / "analyze_loopback_wav.py"
    spec = importlib.util.spec_from_file_location("loopback_reader", source)
    if spec is None or spec.loader is None:
        raise ImportError(source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.read_wav(path)


def _stats(signal: np.ndarray, rate: int) -> dict:
    if signal.ndim == 1:
        signal = signal[:, None]
    mono = np.mean(signal, axis=1, dtype=np.float64)
    peak = float(np.max(np.abs(mono))) if len(mono) else 0.0
    rms = float(np.sqrt(np.mean(mono * mono))) if len(mono) else 0.0
    bands = []
    if len(mono) >= 16:
        window = np.hanning(len(mono))[:, None]
        # Keep channel power before any downmix.  Averaging per-channel power
        # avoids L/R phase cancellation in binaural endpoint captures.
        power = np.mean(np.abs(np.fft.rfft(signal * window, axis=0)) ** 2, axis=1)
        hz = np.fft.rfftfreq(len(mono), 1.0 / rate)
        for low, high in ((0, 80), (80, 200), (200, 375), (375, 1000), (1000, 5000), (5000, 20000)):
            bands.append({"lowHz": low, "highHz": high,
                          "energy": float(power[(hz >= low) & (hz < high)].sum())})
    total = sum(item["energy"] for item in bands)
    for item in bands:
        item["fraction"] = item["energy"] / total if total else 0.0
    return {"frames": int(len(mono)), "peak": peak, "rms": rms,
            "crestFactor": peak / rms if rms else None, "bands": bands,
            "bandMethod": "full-segment Hann FFT; per-channel power mean, no L/R downmix",
            "analyzedBandwidthHz": [0, 20000]}


def compare(loopback: Path, object_direct: Path, onset: int | None = None,
            frames: int | None = None) -> dict:
    captured, capture_rate, capture_encoding = _read_wav(loopback)
    reference, reference_rate, reference_encoding = _read_wav(object_direct)
    if capture_rate != reference_rate:
        raise ValueError("sample rates differ")
    capture_mono = np.mean(captured, axis=1, dtype=np.float64)
    peak = float(np.max(np.abs(capture_mono))) if len(capture_mono) else 0.0
    onset_method = "explicit"
    if onset is None:
        active = np.flatnonzero(np.abs(capture_mono) >= max(1e-6, peak * 1e-3))
        onset = int(active[0]) if len(active) else 0
        onset_method = "first-sample-above-60dB-relative-peak"
    available = min(len(capture_mono) - onset, len(reference))
    count = available if frames is None else min(available, frames)
    if count <= 0:
        raise ValueError("no aligned frames")
    cap = capture_mono[onset:onset + count]
    ref = np.mean(reference[:count], axis=1, dtype=np.float64)
    cap_stats = _stats(captured[onset:onset + count], capture_rate)
    ref_stats = _stats(reference[:count], reference_rate)
    cap_rms = float(np.sqrt(np.mean(cap * cap)))
    ref_rms = float(np.sqrt(np.mean(ref * ref)))
    if cap_rms and ref_rms:
        cap_norm = cap / cap_rms
        ref_norm = ref / ref_rms
        correlation = float(np.corrcoef(cap_norm, ref_norm)[0, 1])
        diff_rms = float(np.sqrt(np.mean((cap_norm - ref_norm) ** 2)))
    else:
        correlation = None; diff_rms = None
    return {
        "schema": "audioplayer.spatial-loopback-object-direct-comparison.v1",
        "result": "PASS_COMPARISON_ONLY" if cap_rms and ref_rms else "INCONCLUSIVE_NO_SIGNAL",
        "evidenceLayer": "endpoint-loopback-vs-renderer-neutral-reference",
        "loopback": {"wav": str(loopback.resolve()), "encoding": capture_encoding,
                      "onsetSample": onset, "onsetMethod": onset_method, "stats": cap_stats},
        "objectDirect": {"wav": str(object_direct.resolve()), "encoding": reference_encoding,
                          "stats": ref_stats},
        "alignedFrames": int(count), "sampleRate": int(capture_rate),
        "rmsNormalized": {"correlation": correlation, "differenceRms": diff_rms},
        "bandFractionComparison": [
            {"lowHz": captured_band["lowHz"], "highHz": captured_band["highHz"],
             "loopbackFraction": captured_band["fraction"],
             "objectDirectFraction": reference_band["fraction"],
             "loopbackToObjectDirectFractionRatio":
                 captured_band["fraction"] / reference_band["fraction"]
                 if reference_band["fraction"] else None,
             "loopbackToObjectDirectFractionDb":
                 10.0 * float(np.log10(captured_band["fraction"] / reference_band["fraction"]))
                 if captured_band["fraction"] > 0 and reference_band["fraction"] > 0 else None}
            for captured_band, reference_band in zip(cap_stats["bands"], ref_stats["bands"])
        ],
        "limitations": ["Loopback may include endpoint/device processing and startup delay.",
                        "RMS normalization is for shape comparison, not level equivalence.",
                        "Object Direct is dual-mono renderer-neutral PCM, not a binaural, spatial endpoint, or subjective reference.",
                        "Band comparisons use total-bandwidth-normalized FFT fractions; they do not compare absolute FFT energy.",
                        "This does not establish that the two paths should be sample-identical."],
    }


class Tests(unittest.TestCase):
    @staticmethod
    def _write(path: Path, x: np.ndarray, rate: int = 48000):
        import struct
        x = np.asarray(x, dtype="<f4"); payload = x.tobytes(); channels = x.shape[1]
        fmt = struct.pack("<HHIIHH", 3, channels, rate, rate * channels * 4, channels * 4, 32)
        def c(k, b): return k + struct.pack("<I", len(b)) + b + (b"\0" if len(b) & 1 else b"")
        body = c(b"fmt ", fmt) + c(b"data", payload)
        path.write_bytes(b"RIFF" + struct.pack("<I", 4 + len(body)) + b"WAVE" + body)

    def test_aligned_comparison(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); x = np.zeros((1000, 2), np.float32); x[20:] = .25
            y = np.zeros((1000, 2), np.float32); y[:10] = .5
            self._write(root / "loop.wav", np.vstack((np.zeros((30, 2)), x)))
            self._write(root / "ref.wav", y)
            result = compare(root / "loop.wav", root / "ref.wav", onset=30, frames=1000)
            self.assertEqual(result["result"], "PASS_COMPARISON_ONLY")
            self.assertEqual(result["alignedFrames"], 1000)
            self.assertAlmostEqual(sum(item["loopbackFraction"] for item in result["bandFractionComparison"]), 1.0)

    def test_band_power_does_not_cancel_opposite_channels(self):
        samples = np.zeros((1024, 2), dtype=np.float32)
        samples[:, 0] = np.sin(np.arange(1024) * 0.2)
        samples[:, 1] = -samples[:, 0]
        stats = _stats(samples, 48000)
        self.assertGreater(sum(item["energy"] for item in stats["bands"]), 0.0)
        self.assertIn("per-channel power", stats["bandMethod"])


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--loopback", type=Path)
    parser.add_argument("--object-direct", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--onset", type=int)
    parser.add_argument("--frames", type=int)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        result = unittest.TextTestRunner(verbosity=1).run(unittest.defaultTestLoader.loadTestsFromTestCase(Tests))
        raise SystemExit(0 if result.wasSuccessful() else 1)
    if not args.loopback or not args.object_direct or not args.output:
        parser.error("--loopback, --object-direct and --output are required unless --self-test is used")
    report = compare(args.loopback, args.object_direct, args.onset, args.frames)
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"result": report["result"], "output": str(args.output.resolve()),
                      "alignedFrames": report["alignedFrames"]}))


if __name__ == "__main__":
    main()
