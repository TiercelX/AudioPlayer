"""Compare short loopback captures from fixed SpatialDynamicProbe positions.

This is an endpoint-capture diagnostic, not an HRTF or listening-quality test.
It deliberately reports silence and indistinguishable captures as stop-worthy
inconclusive outcomes instead of inventing positional evidence.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

import numpy as np


def _load_reader():
    path = Path(__file__).resolve().parents[1] / "tools" / "atmos-render" / "analyze_loopback_wav.py"
    spec = importlib.util.spec_from_file_location("loopback_analyzer", path)
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.read_wav


read_wav = _load_reader()


def _signal_metrics(audio: np.ndarray, rate: int) -> dict:
    mono = np.mean(audio, axis=1, dtype=np.float64)
    peak = float(np.max(np.abs(audio))) if audio.size else 0.0
    threshold = max(1.0e-6, peak * 1.0e-3)
    active = np.flatnonzero(np.max(np.abs(audio), axis=1) >= threshold)
    onset = int(active[0]) if len(active) else None
    normalized = audio / peak if peak else np.zeros_like(audio)
    tails = []
    for db in (20, 40, 60):
        if onset is None:
            tails.append({"db": db, "milliseconds": None, "censored": True,
                          "reason": "no-valid-transient"})
            continue
        level = peak * 10.0 ** (-db / 20.0)
        above = np.flatnonzero(np.max(np.abs(audio[onset:]), axis=1) >= level)
        last = onset + int(above[-1]) if len(above) else onset
        tails.append({"db": db, "milliseconds": 1000.0 * (last - onset) / rate,
                      "censored": bool(last >= len(audio) - 1),
                      "reason": "threshold-crossing"})
    left = audio[:, 0] if audio.shape[1] else mono
    right = audio[:, 1] if audio.shape[1] > 1 else left
    if np.std(left) and np.std(right):
        correlation = float(np.corrcoef(left, right)[0, 1])
    else:
        correlation = None
    return {
        "frames": int(len(audio)),
        "sampleRate": int(rate),
        "firstValidTransientSample": onset,
        "firstValidTransientMilliseconds": 1000.0 * onset / rate if onset is not None else None,
        "peak": peak,
        "normalizedPeak": 1.0 if peak else 0.0,
        "normalizedWaveform": normalized.astype(np.float64),
        "leftEnergy": float(np.sum(left.astype(np.float64) ** 2)),
        "rightEnergy": float(np.sum(right.astype(np.float64) ** 2)),
        "leftRightEnergyRatio": float(np.sum(left.astype(np.float64) ** 2) /
                                       np.sum(right.astype(np.float64) ** 2))
        if np.sum(right.astype(np.float64) ** 2) else None,
        "leftRightCorrelation": correlation,
        "tails": tails,
    }


def compare(inputs: dict[str, Path]) -> dict:
    captures = {}
    for position, path in inputs.items():
        audio, rate, encoding = read_wav(path)
        metrics = _signal_metrics(audio, rate)
        metrics.pop("normalizedWaveform")
        metrics["encoding"] = encoding
        metrics["wav"] = str(path.resolve())
        captures[position] = metrics

    active = [item for item in captures.values() if item["firstValidTransientSample"] is not None]
    pairs = {}
    distinguishable = False
    partial_silence = len(active) != len(captures)
    positions = list(inputs)
    for index, first in enumerate(positions):
        first_audio, first_rate, _ = read_wav(inputs[first])
        for second in positions[index + 1:]:
            second_audio, second_rate, _ = read_wav(inputs[second])
            if first_rate != second_rate:
                raise ValueError("captures have different sample rates")
            first_onset = captures[first]["firstValidTransientSample"]
            second_onset = captures[second]["firstValidTransientSample"]
            if first_onset is None or second_onset is None:
                pairs[f"{first}__{second}"] = {
                    "framesCompared": 0,
                    "alignment": "per-capture-onset",
                    "firstOnsetSample": first_onset,
                    "secondOnsetSample": second_onset,
                    "normalizedMaxAbsDifference": None,
                    "normalizedCorrelation": None,
                }
                continue
            length = min(len(first_audio) - first_onset, len(second_audio) - second_onset)
            a = first_audio[first_onset:first_onset + length]
            b = second_audio[second_onset:second_onset + length]
            peak_a = np.max(np.abs(a))
            peak_b = np.max(np.abs(b))
            if not peak_a or not peak_b:
                corr = None
                difference = 0.0
            else:
                na = a / peak_a
                nb = b / peak_b
                difference = float(np.max(np.abs(na - nb)))
                af = na.reshape(-1)
                bf = nb.reshape(-1)
                corr = float(np.corrcoef(af, bf)[0, 1]) if np.std(af) and np.std(bf) else None
                distinguishable = distinguishable or difference > 0.05 or (corr is not None and corr < 0.995)
            pairs[f"{first}__{second}"] = {
                "framesCompared": int(length),
                "alignment": "per-capture-onset",
                "firstOnsetSample": first_onset,
                "secondOnsetSample": second_onset,
                "normalizedMaxAbsDifference": difference,
                "normalizedCorrelation": corr,
            }

    if not active:
        result = "INCONCLUSIVE_LOOPBACK_SILENT"
        decision = "STOP_NO_VALID_TRANSIENT"
    elif partial_silence:
        result = "INCONCLUSIVE_PARTIAL_SILENCE"
        decision = "STOP_PARTIAL_POSITION_CAPTURE"
    elif not distinguishable:
        result = "INCONCLUSIVE_POSITION_NOT_DISTINGUISHABLE"
        decision = "STOP_POSITION_NOT_DISTINGUISHABLE"
    else:
        result = "PASS_POSITION_DIFFERENTIATED_ENDPOINT_CAPTURE"
        decision = "POSITION_DIFFERENTIATED_AT_LOOPBACK_LAYER_ONLY"
    return {
        "schema": "audioplayer.spatial-impulse-loopback-comparison.v1",
        "result": result,
        "decision": decision,
        "evidenceLayer": "wasapi-loopback-endpoint-capture",
        "alignment": "per-capture-first-valid-transient",
        "captures": captures,
        "pairwise": pairs,
        "limitations": [
            "Loopback endpoint capture is not physical headphone/speaker evidence.",
            "Peak-normalized waveform comparison is not an HRTF or localization oracle.",
            "Threshold tails are transient-envelope evidence; program reverb is not isolated.",
        ],
    }


class SpatialImpulseComparisonTests(unittest.TestCase):
    @staticmethod
    def _write_float_wav(path: Path, samples: np.ndarray, rate: int = 48000) -> None:
        import struct
        samples = np.asarray(samples, dtype="<f4")
        channels = samples.shape[1]
        payload = samples.tobytes()
        fmt = struct.pack("<HHIIHH", 3, channels, rate, rate * channels * 4,
                          channels * 4, 32)
        def chunk(kind: bytes, body: bytes) -> bytes:
            return kind + struct.pack("<I", len(body)) + body + (b"\0" if len(body) & 1 else b"")
        body = chunk(b"fmt ", fmt) + chunk(b"data", payload)
        path.write_bytes(b"RIFF" + struct.pack("<I", 4 + len(body)) + b"WAVE" + body)

    def test_position_difference_and_tail_metrics(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            base = np.zeros((4800, 2), dtype=np.float32)
            front = base.copy(); front[100, :] = 0.5; front[180, :] = 0.1
            left = base.copy(); left[100, 0] = 0.5; left[160, 0] = 0.1
            upper = base.copy(); upper[240, 1] = 0.5
            paths = {}
            for name, data in (("front", front), ("left", left), ("upper", upper)):
                path = root / f"{name}.wav"; self._write_float_wav(path, data); paths[name] = path
            report = compare(paths)
            self.assertEqual(report["result"], "PASS_POSITION_DIFFERENTIATED_ENDPOINT_CAPTURE")
            self.assertEqual(report["captures"]["front"]["firstValidTransientSample"], 100)
            self.assertIn("front__left", report["pairwise"])

    def test_silence_requests_stop(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); paths = {}
            for name in ("front", "left", "upper"):
                path = root / f"{name}.wav"; self._write_float_wav(path, np.zeros((480, 2))); paths[name] = path
            report = compare(paths)
            self.assertEqual(report["result"], "INCONCLUSIVE_LOOPBACK_SILENT")
            self.assertEqual(report["decision"], "STOP_NO_VALID_TRANSIENT")

    def test_partial_silence_cannot_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); paths = {}
            for name in ("front", "left", "upper"):
                path = root / f"{name}.wav"
                samples = np.zeros((480, 2), dtype=np.float32)
                if name != "upper": samples[100, :] = 0.5
                self._write_float_wav(path, samples); paths[name] = path
            report = compare(paths)
            self.assertEqual(report["result"], "INCONCLUSIVE_PARTIAL_SILENCE")

    def test_pairwise_alignment_uses_each_onset(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); paths = {}
            for name, onset in (("front", 10), ("left", 100)):
                samples = np.zeros((480, 2), dtype=np.float32); samples[onset, :] = 0.5
                path = root / f"{name}.wav"; self._write_float_wav(path, samples); paths[name] = path
            report = compare(paths)
            pair = report["pairwise"]["front__left"]
            self.assertEqual(pair["alignment"], "per-capture-onset")
            self.assertEqual((pair["firstOnsetSample"], pair["secondOnsetSample"]), (10, 100))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", action="append", metavar="POSITION=WAV")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        result = unittest.TextTestRunner(verbosity=1).run(
            unittest.defaultTestLoader.loadTestsFromTestCase(SpatialImpulseComparisonTests))
        raise SystemExit(0 if result.wasSuccessful() else 1)
    if not args.input or not args.output:
        parser.error("--input POSITION=WAV (at least one, normally three) and --output are required")
    inputs = {}
    for item in args.input:
        position, separator, path = item.partition("=")
        if not separator or position not in {"front", "left", "upper"}:
            parser.error(f"invalid --input: {item}")
        inputs[position] = Path(path)
    if len(inputs) < 2:
        parser.error("at least two positions are required")
    report = compare(inputs)
    args.output.write_text(json.dumps(report, indent=2, default=lambda value: value.tolist()), encoding="utf-8")
    print(json.dumps({"result": report["result"], "output": str(args.output.resolve())}))
    raise SystemExit(0 if report["result"].startswith("PASS") else 1)


if __name__ == "__main__":
    main()
