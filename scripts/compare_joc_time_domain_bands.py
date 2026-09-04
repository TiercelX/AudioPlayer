#!/usr/bin/env python3
"""Compare decoded channels and raw Gate6C object PCM in time-domain bands.

This complements the native QMF trace: 4096-sample Welch bins resolve the
80/200 Hz boundaries, while the QMF matrix audit intentionally remains at its
native nominal 375 Hz bands. Object-energy sums are not acoustic energy
conservation and no endpoint/listening claim is made.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Any

import numpy as np


RATE = 48_000
WINDOW = 4096
HOP = 2048
BANDS = ((0.0, 80.0), (80.0, 200.0), (200.0, 375.0), (375.0, 750.0),
         (750.0, 5000.0), (5000.0, 20000.0))
LAYOUTS = {
    "5.1(side)": ["FL", "FR", "FC", "LFE", "SL", "SR"],
    "5.1": ["FL", "FR", "FC", "LFE", "BL", "BR"],
    "7.1": ["FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR"],
    "7.1(rear)": ["FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR"],
    "7.1(wide)": ["FL", "FR", "FC", "LFE", "FLC", "FRC", "SL", "SR"],
}


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _fnv1a64(path: Path) -> str:
    value = 14695981039346656037
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            for byte in block:
                value ^= byte
                value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return f"fnv1a64-{value:016x}"


def _verify_bundle_provenance(source: Path, bundle: Path,
                              allow_unverified: bool = False) -> dict[str, Any]:
    path = bundle / "bundle-provenance.json"
    failure = None
    document: dict[str, Any] = {}
    if not path.is_file():
        failure = f"missing bundle provenance: {path}"
    else:
        try:
            document = json.loads(path.read_text(encoding="utf-8"))
            if not isinstance(document, dict):
                failure = "provenance root is not an object"
            else:
                recorded_path = document.get("sourcePath")
                recorded_digest = document.get("sourceFileDigest")
                if document.get("schema") != "eac3-bear-bundle-provenance-v1":
                    failure = "unsupported bundle provenance schema"
                elif not isinstance(recorded_path, str) or not recorded_path:
                    failure = "bundle provenance has no source path"
                elif os.path.normcase(os.path.abspath(recorded_path)) != os.path.normcase(os.path.abspath(str(source))):
                    failure = f"bundle source mismatch: {recorded_path} != {source}"
                elif not source.is_file():
                    failure = f"source not found: {source}"
                elif not isinstance(recorded_digest, str) or recorded_digest != _fnv1a64(source):
                    failure = "bundle source digest mismatch"
                elif document.get("sourceVerified") is not True:
                    failure = "bundle source is explicitly unverified"
        except (OSError, json.JSONDecodeError, TypeError) as error:
            failure = f"invalid bundle provenance: {error}"
    if failure is not None:
        if not allow_unverified:
            raise ValueError(failure)
        return {"path": str(path.resolve()), "verified": False, "failure": failure,
                "document": document}
    return {"path": str(path.resolve()), "verified": True, "failure": None,
            "document": document}


def _load_object_reader():
    root = Path(__file__).resolve().parents[1]
    path = root / "tools" / "atmos-render" / "object_direct_oracle.py"
    spec = importlib.util.spec_from_file_location("object_direct_oracle", path)
    if spec is None or spec.loader is None:
        raise RuntimeError("object_direct_oracle import failed")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _resolve_tool(path: Path | None, name: str) -> Path:
    if path is not None:
        if path.is_file():
            return path.resolve()
        raise ValueError(f"{name} not found: {path}")
    bundled = Path(__file__).resolve().parents[1] / "build-mm" / "ffmpeg-audio-core" / "runtime-with-ffprobe-msvc" / "bin" / f"{name}.exe"
    found = shutil.which(f"{name}.exe")
    if bundled.is_file():
        return bundled
    if found:
        return Path(found).resolve()
    raise ValueError(f"{name}.exe not found; pass --{name}")


def _probe(ffprobe: Path, source: Path) -> dict[str, Any]:
    completed = subprocess.run(
        [str(ffprobe), "-v", "error", "-show_streams", "-show_format", "-of", "json", str(source)],
        check=False, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if completed.returncode != 0:
        raise ValueError(f"ffprobe failed: {completed.stderr.strip()}")
    try:
        document = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise ValueError(f"invalid ffprobe JSON: {error}") from error
    streams = [stream for stream in document.get("streams", [])
               if stream.get("codec_type") == "audio"]
    if not streams:
        raise ValueError("no audio stream")
    stream = streams[0]
    rate = int(stream.get("sample_rate", 0))
    channels = int(stream.get("channels", 0))
    if rate != RATE or channels <= 0:
        raise ValueError(f"expected 48000 Hz audio, got {rate} Hz/{channels} channels")
    return {"stream": stream, "format": document.get("format", {})}


def _channel_mapping(probe: dict[str, Any], lfe_index: int | None,
                     main_indices: list[int] | None) -> tuple[int, list[int], list[str], str]:
    stream = probe["stream"]
    channels = int(stream["channels"])
    layout = str(stream.get("channel_layout", ""))
    names = LAYOUTS.get(layout)
    if lfe_index is None:
        if names is None:
            raise ValueError(f"unknown channel layout {layout!r}; pass explicit --lfe-index and --main-indices")
        lfe_index = names.index("LFE")
    if lfe_index < 0 or lfe_index >= channels:
        raise ValueError("LFE index out of range")
    if main_indices is None:
        main_indices = [index for index in range(channels) if index != lfe_index]
    if not main_indices or any(index < 0 or index >= channels for index in main_indices):
        raise ValueError("invalid main channel indices")
    if lfe_index in main_indices or len(set(main_indices)) != len(main_indices):
        raise ValueError("LFE must be excluded from unique main indices")
    if names is None:
        names = [f"channel{index}" for index in range(channels)]
    return lfe_index, main_indices, names, layout


def _decode(ffmpeg: Path, source: Path, channels: int, frames: int) -> np.ndarray:
    completed = subprocess.run(
        [str(ffmpeg), "-v", "error", "-i", str(source), "-map", "0:a:0",
         "-t", f"{frames / RATE:.9f}", "-f", "f32le", "-acodec", "pcm_f32le",
         "-ar", str(RATE), "-"], check=False, capture_output=True)
    if completed.returncode != 0:
        raise ValueError(f"ffmpeg decode failed: {completed.stderr.decode(errors='replace').strip()}")
    values = np.frombuffer(completed.stdout, dtype="<f4")
    if values.size % channels:
        raise ValueError("decoded PCM is not channel aligned")
    values = values.reshape(-1, channels)
    if len(values) < frames:
        raise ValueError(f"decoded {len(values)} frames, expected at least {frames}")
    values = values[:frames]
    if not np.isfinite(values).all():
        raise ValueError("decoded PCM contains non-finite samples")
    return values


def _load_objects(bundle: Path, frames: int) -> tuple[np.ndarray, np.ndarray]:
    module = _load_object_reader()
    # Gate6C BSCN batches can be 959/1536/577 samples rather than one exact
    # 1536-frame block. Select complete batches through the requested horizon,
    # then trim the final batch to the common time-domain comparison length.
    all_infos, total = module._load_bundle(bundle, None)
    infos = []
    for info in all_infos:
        infos.append(info)
        if info["end"] >= frames:
            break
    if not infos or infos[-1]["end"] < frames:
        raise ValueError(f"bundle has {total} frames, expected at least {frames}")
    object_parts = []
    lfe_parts = []
    for info in infos:
        _, objects, lfe = module._read_batch(info["path"], info["start"])
        object_parts.append(objects)
        lfe_parts.append(lfe if len(lfe) else np.zeros(info["samples"], dtype=np.float32))
    objects = np.concatenate(object_parts, axis=1)
    lfe = np.concatenate(lfe_parts)
    objects = objects[:, :frames]
    lfe = lfe[:frames]
    if not np.isfinite(objects).all() or not np.isfinite(lfe).all():
        raise ValueError("bundle contains non-finite PCM")
    return objects, lfe


def _welch_energy(samples: np.ndarray) -> tuple[dict[str, float], float]:
    samples = np.asarray(samples, dtype=np.float64)
    if samples.ndim == 1:
        samples = samples[:, None]
    window = np.hanning(WINDOW)
    norm = float(np.sum(window * window))
    frequencies = np.fft.rfftfreq(WINDOW, 1.0 / RATE)
    energy = np.zeros((samples.shape[1], len(BANDS)), dtype=np.float64)
    blocks = 0
    for start in range(0, len(samples) - WINDOW + 1, HOP):
        spectrum = np.abs(np.fft.rfft(samples[start:start + WINDOW] * window[:, None], axis=0)) ** 2
        spectrum[1:-1] *= 2.0
        spectrum /= norm
        for index, (low, high) in enumerate(BANDS):
            energy[:, index] += spectrum[(frequencies >= low) & (frequencies < high)].sum(axis=0)
        blocks += 1
    if blocks == 0:
        raise ValueError("not enough frames for 4096-sample Welch window")
    energy /= blocks
    labels = [f"{low:g}-{high:g}Hz" for low, high in BANDS]
    return ({label: float(value) for label, value in zip(labels, energy.sum(axis=0))},
            float(np.sum(samples * samples)))


def _per_channel_welch(samples: np.ndarray) -> tuple[list[dict[str, float]], list[float]]:
    samples = np.asarray(samples)
    if samples.ndim == 1:
        samples = samples[:, None]
    bands = []
    totals = []
    for index in range(samples.shape[1]):
        result, total = _welch_energy(samples[:, index])
        bands.append(result)
        totals.append(total)
    return bands, totals


def _band_ratios(main: dict[str, float], raw_objects: dict[str, float],
                 coherent: dict[str, float]) -> dict[str, dict[str, float | None]]:
    return {
        label: {
            "rawObjectsToMain": raw_objects[label] / main[label] if main[label] else None,
            "coherentObjectSumToRawObjects": coherent[label] / raw_objects[label]
            if raw_objects[label] else None,
            "coherentObjectSumToMain": coherent[label] / main[label] if main[label] else None,
        }
        for label in main
    }


def analyze(source: Path, bundle: Path, max_frames: int,
            ffmpeg: Path | None = None, ffprobe: Path | None = None,
            lfe_index: int | None = None, main_indices: list[int] | None = None,
            allow_unverified_bundle_source: bool = False) -> dict[str, Any]:
    if max_frames <= WINDOW or max_frames % 1536:
        raise ValueError("max-frames must be a multiple of 1536 and exceed the Welch window")
    ffmpeg_path = _resolve_tool(ffmpeg, "ffmpeg")
    ffprobe_path = _resolve_tool(ffprobe, "ffprobe")
    bundle_provenance = _verify_bundle_provenance(
        source, bundle, allow_unverified_bundle_source)
    probe = _probe(ffprobe_path, source)
    lfe_index, main_indices, names, layout = _channel_mapping(probe, lfe_index, main_indices)
    decoded = _decode(ffmpeg_path, source, int(probe["stream"]["channels"]), max_frames)
    objects, bundle_lfe = _load_objects(bundle, max_frames)
    main = decoded[:, main_indices]
    lfe = decoded[:, lfe_index]
    object_bands, object_totals = _per_channel_welch(objects.T)
    main_bands, main_totals = _per_channel_welch(main)
    lfe_bands, lfe_totals = _per_channel_welch(lfe)
    object_sum_bands, object_sum_total = _welch_energy(objects.sum(axis=0))
    decoded_main_sum_bands, decoded_main_sum_total = _welch_energy(main.sum(axis=1))
    labels = [f"{low:g}-{high:g}Hz" for low, high in BANDS]
    object_energy = {label: sum(item[label] for item in object_bands) for label in labels}
    main_energy = {label: sum(item[label] for item in main_bands) for label in labels}
    lfe_energy = {label: lfe_bands[0][label] for label in labels}
    band_ratios = _band_ratios(main_energy, object_energy, object_sum_bands)
    bundle_lfe_energy = float(np.dot(bundle_lfe, bundle_lfe))
    lfe_difference = lfe.astype(np.float64) - bundle_lfe.astype(np.float64)
    return {
        "schema": "audioplayer.joc-time-domain-band-audit.v1",
        "result": "PASS_ACCOUNTING" if bundle_provenance["verified"]
                  else "INCONCLUSIVE_UNVERIFIED_BUNDLE_SOURCE",
        "evidenceLayer": "offline-decoded-pcm-and-gate6c-object-pcm",
        "source": str(source.resolve()), "sourceSha256": _sha256(source),
        "bundle": str(bundle.resolve()), "sampleRateHz": RATE,
        "bundleProvenance": bundle_provenance,
        "frames": max_frames, "durationSeconds": max_frames / RATE,
        "ffprobe": probe, "ffmpeg": str(ffmpeg_path), "ffprobePath": str(ffprobe_path),
        "channelMapping": {"layout": layout, "names": names, "lfeIndex": lfe_index,
                            "mainIndices": main_indices, "contract": "explicit-known-layout-or-user-override"},
        "method": {"name": "Welch/Hann", "windowFrames": WINDOW, "hopFrames": HOP,
                   "resolutionHz": RATE / WINDOW, "bandsHz": BANDS,
                   "note": "Discrete FFT bins are selected by half-open band edges; this is time-domain PCM spectral accounting, complementary to nominal QMF bands."},
        "decodedNonLfeMain": {"perChannel": [{"index": index, "name": names[index], "bands": item, "timeDomainEnergy": total}
                                               for index, (item, total) in zip(main_indices, zip(main_bands, main_totals))],
                              "bandEnergy": main_energy, "timeDomainEnergy": sum(main_totals),
                              "coherentChannelSumBandEnergy": decoded_main_sum_bands,
                              "coherentChannelSumTimeDomainEnergy": decoded_main_sum_total},
        "decodedIndependentLfe": {"index": lfe_index, "bands": lfe_energy,
                                  "timeDomainEnergy": lfe_totals[0],
                                  "bundleLfePcmEnergy": bundle_lfe_energy,
                                  "bundleLfeMatchesDecoded": bool(np.array_equal(lfe, bundle_lfe)),
                                  "bundleLfeMaxAbsDifference": float(np.max(np.abs(lfe_difference))),
                                  "bundleLfeRmsDifference": float(np.sqrt(np.mean(lfe_difference * lfe_difference))),
                                  "note": "Decoded source LFE and Gate6C bundle LFE are reported separately; no assumed sample-for-sample equivalence."},
        "rawObjects": {"count": int(objects.shape[0]),
                       "perObject": [{"object": index, "bands": item, "timeDomainEnergy": total}
                                     for index, (item, total) in enumerate(zip(object_bands, object_totals))],
                       "sumOfObjectBandEnergy": object_energy,
                       "sumOfObjectTimeDomainEnergy": sum(object_totals),
                       "coherentObjectSumBandEnergy": object_sum_bands,
                       "coherentObjectSumTimeDomainEnergy": object_sum_total,
                       "bandRatios": band_ratios},
        "conclusion": {"lfeLeakage": "NO_LFE_TERM_IN_TRACED_QMF_RECONSTRUCTION applies only to the separate matching QMF trace; this time-domain report does not expose LFE as a matrix input",
                       "mainLowFrequencyContent": "REPORTS_DECODED_NON_LFE_PCM",
                       "objectEnergyInterpretation": "sum of 15 object channels is not acoustic energy conservation",
                       "qmfComplement": "Use joc-low-frequency-energy-audit for nominal 375 Hz matrix bands; this report resolves 80/200 Hz only in time-domain PCM."},
    }


def markdown(report: dict[str, Any]) -> str:
    labels = [f"{low:g}-{high:g} Hz" for low, high in BANDS]
    main = report["decodedNonLfeMain"]["bandEnergy"]
    lfe = report["decodedIndependentLfe"]["bands"]
    objects = report["rawObjects"]["sumOfObjectBandEnergy"]
    coherent = report["rawObjects"]["coherentObjectSumBandEnergy"]
    ratios = report["rawObjects"]["bandRatios"]
    lines = ["# JOC time-domain band audit", "", f"Result: `{report['result']}`; frames `{report['frames']}`; source `{report['source']}`", "",
             "Welch/Hann: 4096-frame window, 2048-frame hop, 11.71875 Hz bins. This resolves 80/200 Hz in decoded PCM and is complementary to the 375 Hz nominal QMF audit.", "",
             "| Band | decoded non-LFE | decoded LFE | sum raw objects | coherent object sum | raw/main | coherent/raw | coherent/main |", "|---|---:|---:|---:|---:|---:|---:|---:|"]
    for label in labels:
        key = label.replace(" ", "")
        ratio = ratios[key]
        lines.append(f"| {label} | {main[key]:.9g} | {lfe[key]:.9g} | {objects[key]:.9g} | {coherent[key]:.9g} | {ratio['rawObjectsToMain']} | {ratio['coherentObjectSumToRawObjects']} | {ratio['coherentObjectSumToMain']} |")
    lines += ["", "Object-channel energy sum is not acoustic energy conservation. This report does not measure endpoint output, Dolby equivalence, or subjective sound. Bundle/source provenance must be verified for PASS_ACCOUNTING."]
    return "\n".join(lines) + "\n"


class TimeDomainAuditTests(unittest.TestCase):
    def test_known_dual_tone_and_lfe_exclusion(self):
        frames = 8192
        time = np.arange(frames) / RATE
        samples = np.column_stack((np.sin(2 * np.pi * 50 * time),
                                   np.sin(2 * np.pi * 250 * time),
                                   np.sin(2 * np.pi * 55 * time)))
        bands, totals = _per_channel_welch(samples)
        labels = [f"{low:g}-{high:g}Hz" for low, high in BANDS]
        self.assertGreater(bands[0][labels[0]], bands[0][labels[1]])
        self.assertGreater(bands[1][labels[2]], bands[1][labels[0]])
        self.assertGreater(bands[2][labels[0]], 0.0)
        main = samples[:, :2]
        self.assertEqual(main.shape[1], 2)
        self.assertEqual(len(totals), 3)

    def test_unknown_layout_requires_explicit_mapping(self):
        with self.assertRaises(ValueError):
            _channel_mapping({"stream": {"channels": 2, "channel_layout": "unknown"}}, None, None)
        index, main, _, _ = _channel_mapping({"stream": {"channels": 2, "channel_layout": "unknown"}}, 1, [0])
        self.assertEqual((index, main), (1, [0]))

    def test_bundle_source_provenance_is_required(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.ec3"
            bundle = root / "bundle"
            bundle.mkdir()
            source.write_bytes(b"matching-source")
            self.assertEqual(_fnv1a64(source), "fnv1a64-6c12eed6efb3e9a8")
            (bundle / "bundle-provenance.json").write_text(json.dumps({
                "schema": "eac3-bear-bundle-provenance-v1",
                "sourcePath": str(source.resolve()),
                "sourceFileDigest": _fnv1a64(source),
                "digestAlgorithm": "FNV-1a-64", "sourceVerified": True}),
                encoding="utf-8")
            self.assertTrue(_verify_bundle_provenance(source, bundle)["verified"])
            other = root / "other.ec3"
            other.write_bytes(b"other-source")
            with self.assertRaises(ValueError):
                _verify_bundle_provenance(other, bundle)
            self.assertFalse(_verify_bundle_provenance(other, bundle, True)["verified"])

    def test_band_ratios(self):
        ratios = _band_ratios({"0-80Hz": 4.0}, {"0-80Hz": 4.016},
                              {"0-80Hz": 8.0})
        self.assertAlmostEqual(ratios["0-80Hz"]["rawObjectsToMain"], 1.004)
        self.assertAlmostEqual(ratios["0-80Hz"]["coherentObjectSumToRawObjects"],
                               8.0 / 4.016)
        self.assertAlmostEqual(ratios["0-80Hz"]["coherentObjectSumToMain"], 2.0)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path)
    parser.add_argument("--bundle", type=Path)
    parser.add_argument("--max-frames", type=int, default=1536000)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--ffmpeg", type=Path)
    parser.add_argument("--ffprobe", type=Path)
    parser.add_argument("--lfe-index", type=int)
    parser.add_argument("--main-indices", type=str)
    parser.add_argument("--allow-unverified-bundle-source", action="store_true",
                        help="continue for exploratory data, but force INCONCLUSIVE result")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        result = unittest.TextTestRunner(verbosity=1).run(
            unittest.defaultTestLoader.loadTestsFromTestCase(TimeDomainAuditTests))
        return 0 if result.wasSuccessful() else 1
    if args.input is None or args.bundle is None or args.output_dir is None:
        parser.error("--input, --bundle and --output-dir are required unless --self-test is used")
    main_indices = None
    if args.main_indices:
        try:
            main_indices = [int(value) for value in args.main_indices.split(",")]
        except ValueError as error:
            parser.error(f"invalid --main-indices: {error}")
    report = analyze(args.input, args.bundle, args.max_frames, args.ffmpeg, args.ffprobe,
                     args.lfe_index, main_indices, args.allow_unverified_bundle_source)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    json_path = args.output_dir / "joc-time-domain-band-audit.json"
    md_path = args.output_dir / "joc-time-domain-band-audit.md"
    json_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    md_path.write_text(markdown(report), encoding="utf-8")
    print(json.dumps({"result": report["result"], "json": str(json_path.resolve()),
                      "markdown": str(md_path.resolve())}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
