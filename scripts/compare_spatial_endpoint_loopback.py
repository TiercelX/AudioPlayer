"""Compare two Windows Spatial endpoint loopback captures.

This is deliberately distinct from the Object Direct comparator: both inputs
are endpoint captures, and each capture is independently onset-aligned before
the common-window statistics are calculated.  It is not an endpoint-quality,
environment-equivalence, HRTF, or listening oracle.
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


BANDS = ((0, 80), (80, 200), (200, 375), (375, 1000),
         (1000, 5000), (5000, 20000))


def _onset(signal: np.ndarray) -> tuple[int, str]:
    if signal.ndim == 1:
        signal = signal[:, None]
    envelope = np.max(np.abs(signal), axis=1) if len(signal) else np.array([])
    peak = float(np.max(envelope)) if len(envelope) else 0.0
    if peak <= 0.0:
        return 0, "no-signal"
    active = np.flatnonzero(envelope >= max(1e-6, peak * 1e-3))
    return (int(active[0]), "first-sample-above-60dB-relative-peak") if len(active) else (0, "no-transient")


def _stats(signal: np.ndarray, rate: int) -> dict:
    if signal.ndim == 1:
        signal = signal[:, None]
    frames = len(signal)
    window = np.hanning(frames)[:, None] if frames >= 16 else None
    bands = []
    if window is not None:
        power = np.mean(np.abs(np.fft.rfft(signal * window, axis=0)) ** 2, axis=1)
        hz = np.fft.rfftfreq(frames, 1.0 / rate)
        for low, high in BANDS:
            bands.append({"lowHz": low, "highHz": high,
                          "energy": float(power[(hz >= low) & (hz < high)].sum())})
    total = sum(item["energy"] for item in bands)
    for item in bands:
        item["fraction"] = item["energy"] / total if total else 0.0
    return {
        "frames": int(frames),
        "channels": int(signal.shape[1]),
        "peakPerChannel": [float(np.max(np.abs(signal[:, c]))) if frames else 0.0
                           for c in range(signal.shape[1])],
        "rmsPerChannel": [float(np.sqrt(np.mean(signal[:, c] ** 2))) if frames else 0.0
                          for c in range(signal.shape[1])],
        "bands": bands,
        "bandMethod": "full-segment Hann FFT; mean per-channel power, 0-20 kHz analyzed bandwidth",
    }


def compare(candidate: Path, reference: Path, candidate_label: str,
            reference_label: str, candidate_renderer: str = "unknown",
            reference_renderer: str = "unknown", candidate_environment: str = "unknown",
            reference_environment: str = "unknown") -> dict:
    cand, cand_rate, cand_encoding = _read_wav(candidate)
    ref, ref_rate, ref_encoding = _read_wav(reference)
    if cand_rate != ref_rate:
        raise ValueError("sample rates differ")
    if cand.ndim == 1:
        cand = cand[:, None]
    if ref.ndim == 1:
        ref = ref[:, None]
    if cand.shape[1] != ref.shape[1]:
        raise ValueError("channel counts differ")
    cand_onset, cand_onset_method = _onset(cand)
    ref_onset, ref_onset_method = _onset(ref)
    cand_active = cand[cand_onset:]
    ref_active = ref[ref_onset:]
    frames = min(len(cand_active), len(ref_active))
    if frames <= 0:
        return {"schema": "audioplayer.endpoint-loopback-vs-endpoint-loopback-comparison.v1",
                "result": "INCONCLUSIVE_NO_COMMON_SIGNAL"}
    cand_active = cand_active[:frames].astype(np.float64)
    ref_active = ref_active[:frames].astype(np.float64)
    correlations = []
    difference_rms = []
    for channel in range(cand.shape[1]):
        x, y = cand_active[:, channel], ref_active[:, channel]
        xr, yr = float(np.sqrt(np.mean(x * x))), float(np.sqrt(np.mean(y * y)))
        if xr and yr:
            xn, yn = x / xr, y / yr
            correlations.append(float(np.corrcoef(xn, yn)[0, 1]))
            difference_rms.append(float(np.sqrt(np.mean((xn - yn) ** 2))))
        else:
            correlations.append(None)
            difference_rms.append(None)
    cand_stats = _stats(cand_active, cand_rate)
    ref_stats = _stats(ref_active, ref_rate)
    return {
        "schema": "audioplayer.endpoint-loopback-vs-endpoint-loopback-comparison.v1",
        "result": "PASS_COMPARISON_ONLY" if any(v is not None for v in correlations)
                  else "INCONCLUSIVE_NO_SIGNAL",
        "evidenceLayer": "endpoint-loopback-vs-endpoint-loopback-signal-statistics",
        "candidate": {"label": candidate_label, "renderer": candidate_renderer,
                      "environment": candidate_environment, "wav": str(candidate.resolve()),
                      "encoding": cand_encoding, "onsetSample": cand_onset,
                      "onsetMethod": cand_onset_method, "stats": cand_stats},
        "reference": {"label": reference_label, "renderer": reference_renderer,
                      "environment": reference_environment, "wav": str(reference.resolve()),
                      "encoding": ref_encoding, "onsetSample": ref_onset,
                      "onsetMethod": ref_onset_method, "stats": ref_stats},
        "sampleRate": int(cand_rate),
        "alignedFrames": int(frames),
        "onsetAlignment": "each capture independently aligned to its own first active sample",
        "normalizedPerChannel": {"correlation": correlations,
                                  "differenceRms": difference_rms},
        "rmsRatioPerChannel": [
            (ref_stats["rmsPerChannel"][i] / cand_stats["rmsPerChannel"][i]
             if cand_stats["rmsPerChannel"][i] else None)
            for i in range(cand.shape[1])],
        "bandFractionComparison": [
            {"lowHz": c["lowHz"], "highHz": c["highHz"],
             "candidateFraction": c["fraction"],
             "referenceFraction": r["fraction"],
             "referenceToCandidateRatio": (r["fraction"] / c["fraction"]
                                            if c["fraction"] else None),
             "referenceToCandidateDb": (10.0 * float(np.log10(r["fraction"] / c["fraction"]))
                                        if c["fraction"] > 0 and r["fraction"] > 0 else None)}
            for c, r in zip(cand_stats["bands"], ref_stats["bands"])
        ],
        "limitations": [
            "Both inputs are endpoint loopback captures; neither is a renderer-neutral or acoustic reference.",
            "Independent onset alignment removes startup offset but cannot remove device, endpoint, or program-state differences.",
            "Normalized waveform correlation and band fractions describe signal similarity only; they do not establish environment equivalence, HRTF correctness, or listening quality.",
            "The analyzed frequency bandwidth is 0-20 kHz and uses per-channel power, not L/R downmix."
        ],
    }


class Tests(unittest.TestCase):
    @staticmethod
    def _write(path: Path, data: np.ndarray, rate: int = 48000):
        import struct
        data = np.asarray(data, dtype="<f4")
        channels = data.shape[1]
        fmt = struct.pack("<HHIIHH", 3, channels, rate, rate * channels * 4,
                          channels * 4, 32)
        def chunk(tag, body):
            return tag + struct.pack("<I", len(body)) + body + (b"\0" if len(body) & 1 else b"")
        body = chunk(b"fmt ", fmt) + chunk(b"data", data.tobytes())
        path.write_bytes(b"RIFF" + struct.pack("<I", 4 + len(body)) + b"WAVE" + body)

    def test_independent_onsets_and_labels(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            base = np.zeros((1000, 2), np.float32)
            base[30:80] = 0.25
            shifted = np.zeros((1020, 2), np.float32)
            shifted[50:100] = 0.5
            self._write(root / "candidate.wav", base)
            self._write(root / "reference.wav", shifted)
            report = compare(root / "candidate.wav", root / "reference.wav", "small", "standard", "hrtf", "standard", "small", "n/a")
            self.assertEqual(report["result"], "PASS_COMPARISON_ONLY")
            self.assertEqual(report["candidate"]["onsetSample"], 30)
            self.assertEqual(report["reference"]["onsetSample"], 50)
            self.assertEqual(report["candidate"]["label"], "small")

    def test_silent_is_inconclusive(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            silent = np.zeros((100, 2), np.float32)
            self._write(root / "a.wav", silent)
            self._write(root / "b.wav", silent)
            self.assertEqual(compare(root / "a.wav", root / "b.wav", "a", "b")["result"],
                             "INCONCLUSIVE_NO_SIGNAL")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--candidate", type=Path)
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--candidate-label", required=False, default="candidate")
    parser.add_argument("--reference-label", required=False, default="reference")
    parser.add_argument("--candidate-renderer", default="unknown")
    parser.add_argument("--reference-renderer", default="unknown")
    parser.add_argument("--candidate-environment", default="unknown")
    parser.add_argument("--reference-environment", default="unknown")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        result = unittest.TextTestRunner(verbosity=1).run(unittest.defaultTestLoader.loadTestsFromTestCase(Tests))
        raise SystemExit(0 if result.wasSuccessful() else 1)
    if not args.candidate or not args.reference or not args.output:
        parser.error("--candidate, --reference and --output are required unless --self-test is used")
    report = compare(args.candidate, args.reference, args.candidate_label, args.reference_label,
                     args.candidate_renderer, args.reference_renderer,
                     args.candidate_environment, args.reference_environment)
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"result": report["result"], "output": str(args.output.resolve()),
                      "alignedFrames": report.get("alignedFrames", 0)}))


if __name__ == "__main__":
    main()
