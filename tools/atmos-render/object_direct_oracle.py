"""Renderer-neutral diagnostic mixer for a Gate6C BSCN object bundle.

This is intentionally not a binaural or Windows Spatial renderer.  It applies
the current per-object volume policy, sums the 15 mono object planes to both
stereo channels, and reports whether low frequency/reverberant content already
exists in the decoded object PCM.  LFE is excluded by default and can only be
added explicitly for an A/B diagnostic.
"""

from __future__ import annotations

import argparse
import bisect
import json
import math
import struct
import tempfile
import unittest
import wave
from pathlib import Path

import numpy as np


RATE = 48_000
OBJECTS = 15
DYNAMIC_HEADROOM_DB = 15.0
DYNAMIC_VOLUME = 10.0 ** (-DYNAMIC_HEADROOM_DB / 20.0)
BANDS = ((0.0, 80.0), (80.0, 200.0), (200.0, 1000.0),
         (1000.0, 5000.0), (5000.0, 20000.0))


def _finite(value: float) -> float:
    if not math.isfinite(float(value)):
        raise ValueError("non-finite numeric value")
    return float(value)


def _state_for_update(update: dict) -> dict:
    """Return the adapter's audible state, retaining flags and gainDb."""
    gain_db = _finite(update.get("gainDb", 0.0))
    active = bool(update.get("active", True))
    minus_infinity = bool(update.get("gainMinusInfinity", False))
    silent = not active or minus_infinity
    volume = 0.0 if silent else DYNAMIC_VOLUME * 10.0 ** (gain_db / 20.0)
    return {"active": active, "minusInfinity": minus_infinity,
            "gainDb": gain_db, "volume": float(volume)}


def _target_gain(update: dict, lfe_gain: float = 1.0) -> float:
    """Compatibility helper returning the target linear volume."""
    return _state_for_update(update)["volume"] * _finite(lfe_gain)


def _silent(state: dict) -> bool:
    return not state["active"] or state["minusInfinity"]


def _interpolate_state(start: dict, target: dict, fraction: float) -> dict:
    """Match SpatialPropertyAdapter::interpolate for gain state."""
    if fraction <= 0.0:
        return dict(start)
    if fraction >= 1.0:
        return dict(target)
    result = dict(target)
    result["active"] = start["active"]
    result["minusInfinity"] = start["minusInfinity"]
    if not _silent(start) and not _silent(target):
        result["gainDb"] = start["gainDb"] + fraction * (target["gainDb"] - start["gainDb"])
        result["volume"] = DYNAMIC_VOLUME * 10.0 ** (result["gainDb"] / 20.0)
    else:
        result["gainDb"] = target["gainDb"] if fraction >= 1.0 else start["gainDb"]
        result["volume"] = start["volume"] + fraction * (target["volume"] - start["volume"])
        if result["volume"] > 0.0:
            result["active"] = True
            result["minusInfinity"] = False
    return result


def _read_batch(path: Path, expected_start: int | None = None) -> tuple[dict, np.ndarray, np.ndarray]:
    with path.open("rb") as stream:
        header = stream.read(32)
        if len(header) != 32 or header[:4] != b"BSCN":
            raise ValueError(f"invalid BSCN header: {path}")
        version, objects, samples = struct.unpack("<III", header[4:16])
        start, end = struct.unpack("<qq", header[16:32])
        if version != 2 or objects != OBJECTS or samples <= 0 or end != start + samples:
            raise ValueError(f"invalid BSCN dimensions: {path}")
        if expected_start is not None and start != expected_start:
            raise ValueError(f"non-contiguous BSCN: {path.name}, expected {expected_start}, got {start}")
        audio_bytes = OBJECTS * samples * 4
        raw = stream.read(audio_bytes)
        if len(raw) != audio_bytes:
            raise ValueError(f"truncated object PCM: {path}")
        audio = np.frombuffer(raw, dtype="<f4").reshape(OBJECTS, samples)
        lfe_header = stream.read(4)
        if len(lfe_header) != 4:
            raise ValueError(f"missing LFE header: {path}")
        lfe_samples = struct.unpack("<I", lfe_header)[0]
        lfe_raw = stream.read(lfe_samples * 4)
        if len(lfe_raw) != lfe_samples * 4:
            raise ValueError(f"truncated LFE PCM: {path}")
        lfe = np.frombuffer(lfe_raw, dtype="<f4")
        if lfe_samples not in (0, samples):
            raise ValueError(f"LFE/object length mismatch in {path.name}")
    if not np.isfinite(audio).all() or not np.isfinite(lfe).all():
        raise ValueError(f"non-finite PCM in {path.name}")
    return {"path": path, "start": start, "end": end, "samples": samples}, audio, lfe


def _load_bundle(bundle: Path, max_samples: int | None = None):
    infos = []
    expected = 0
    for path in sorted(bundle.glob("batch-*.bin")):
        info, _, _ = _read_batch(path, expected)
        if max_samples is not None and info["start"] >= max_samples:
            break
        if max_samples is not None and info["end"] > max_samples:
            raise ValueError("max-samples must end on a BSCN batch boundary")
        infos.append(info)
        expected = info["end"]
    if not infos:
        raise ValueError("bundle contains no usable batches")
    return infos, expected


def _load_metadata(path: Path, total_samples: int) -> dict[int, list[dict]]:
    events = {index: [] for index in range(1, OBJECTS + 1)}
    if not path.exists():
        raise ValueError(f"metadata.jsonl is required: {path}")
    with path.open(encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            record = json.loads(line)
            for update in record.get("updates", []):
                index = int(update.get("objectIndex", 0))
                position = int(update.get("sourcePosition", -1))
                if index not in events or position < 0:
                    raise ValueError(f"invalid metadata update at line {line_number}")
                if position > total_samples:
                    continue
                events[index].append(update)
    for index in events:
        # Gate7B permits one ordered object state per source position.  Keep
        # the last record if a malformed/overlapping exporter emitted a
        # duplicate position, matching the state that survives the batch.
        deduplicated = {}
        for event in events[index]:
            deduplicated[int(event["sourcePosition"])] = event
        events[index] = [deduplicated[position] for position in sorted(deduplicated)]
        if not events[index] or int(events[index][0]["sourcePosition"]) != 0:
            raise ValueError(f"no metadata updates for object {index}")
    return events


def _prepare_ramp_events(events: list[dict], total_samples: int) -> list[dict]:
    """Build transitions matching SpatialPropertyAdapter's ramp semantics."""
    if not events or int(events[0]["sourcePosition"]) != 0:
        raise ValueError("metadata timeline must start at sample zero")
    initial = _state_for_update(events[0])
    prepared = [{"position": 0, "start": initial, "target": initial,
                 "duration": 0}]
    for event in events[1:]:
        position = int(event["sourcePosition"])
        if position > total_samples:
            break
        previous = prepared[-1]
        elapsed = max(0, position - int(previous["position"]))
        duration = int(previous["duration"])
        fraction = elapsed / float(duration) if duration else 1.0
        current = _interpolate_state(previous["start"], previous["target"], fraction)
        prepared.append({"position": position, "start": current,
                         "target": _state_for_update(event),
                         "duration": max(0, int(event.get("rampDuration", 0)))})
    return prepared


def _gain_prepared_segment(prepared: list[dict], start: int, count: int,
                           positions: list[int] | None = None) -> np.ndarray:
    """Evaluate prepared ramps, including chunk starts inside an active ramp."""
    if count <= 0:
        return np.empty(0, dtype=np.float32)
    end = start + count
    positions = positions or [int(event["position"]) for event in prepared]
    if start < positions[0]:
        raise ValueError("gain evaluation precedes first metadata state")
    output = np.empty(count, dtype=np.float32)
    first = max(0, bisect.bisect_right(positions, start) - 1)
    last = min(len(prepared), bisect.bisect_left(positions, end) + 1)
    for number in range(first, last):
        event = prepared[number]
        event_at = int(event["position"])
        next_at = end if number + 1 >= len(prepared) else min(end, int(prepared[number + 1]["position"]))
        fill_start = max(start, event_at)
        fill_end = min(end, next_at)
        if fill_end <= fill_start:
            continue
        relative = np.arange(fill_start, fill_end, dtype=np.float64) - event_at
        ramp = int(event["duration"])
        if ramp > 0:
            fractions = np.clip(relative / ramp, 0.0, 1.0)
            values = np.asarray([_interpolate_state(event["start"], event["target"], float(f))["volume"]
                                 for f in fractions], dtype=np.float64)
        else:
            values = np.full(fill_end - fill_start, float(event["target"]["volume"]))
        output[fill_start - start:fill_end - start] = values.astype(np.float32)
    if not np.isfinite(output).all():
        raise ValueError("gain evaluation left non-finite samples")
    return output


def _gain_segment(events: list[dict], start: int, count: int) -> np.ndarray:
    """Prepare and evaluate one interval; convenient for focused tests."""
    return _gain_prepared_segment(_prepare_ramp_events(events, start + count), start, count)


class _Stats:
    def __init__(self) -> None:
        self.frames = 0
        self.sum_squares = 0.0
        self.peak = 0.0
        self.nonzero_frames = 0
        self.clip_samples = 0
        self.band_energy = np.zeros(len(BANDS), dtype=np.float64)

    def add(self, samples: np.ndarray) -> None:
        samples = np.asarray(samples, dtype=np.float64)
        if samples.ndim != 1 or not np.isfinite(samples).all():
            raise ValueError("non-finite or invalid diagnostic PCM")
        self.frames += len(samples)
        self.sum_squares += float(np.dot(samples, samples))
        self.peak = max(self.peak, float(np.max(np.abs(samples))) if len(samples) else 0.0)
        self.nonzero_frames += int(np.count_nonzero(samples))
        self.clip_samples += int(np.count_nonzero(np.abs(samples) > 1.0))
        if len(samples):
            window = np.hanning(len(samples))
            spectrum = np.fft.rfft(samples * window)
            frequencies = np.fft.rfftfreq(len(samples), 1.0 / RATE)
            power = np.abs(spectrum) ** 2
            for index, (low, high) in enumerate(BANDS):
                self.band_energy[index] += float(power[(frequencies >= low) & (frequencies < high)].sum())

    def finish(self) -> dict:
        rms = math.sqrt(self.sum_squares / self.frames) if self.frames else 0.0
        total_band = float(self.band_energy.sum())
        return {
            "frames": self.frames,
            "durationSeconds": self.frames / RATE,
            "peak": self.peak,
            "rms": rms,
            "crestFactor": self.peak / rms if rms else None,
            "nonzeroSamples": self.nonzero_frames,
            "clipSamples": self.clip_samples,
            "lufs": None,
            "frequencyMethod": "per-BSCN-block Hann-windowed real FFT; band bins are summed and are not a calibrated filterbank",
            "bandEnergy": [
                {"lowHz": low, "highHz": high, "energy": float(self.band_energy[index]),
                 "fraction": float(self.band_energy[index] / total_band) if total_band else 0.0}
                for index, (low, high) in enumerate(BANDS)
            ],
            "lufsMethod": "not-computed; install pyloudnorm only in an isolated environment if needed",
        }


def _write_wav(path: Path, samples: np.ndarray) -> None:
    samples = np.asarray(samples, dtype=np.float32)
    if samples.ndim != 2 or samples.shape[1] != 2 or not np.isfinite(samples).all():
        raise ValueError("invalid stereo output")
    with wave.open(str(path), "wb") as output:
        output.setnchannels(2)
        output.setsampwidth(4)
        output.setframerate(RATE)
        output.setcomptype("NONE", "not compressed")
        output.writeframes(samples.astype("<f4", copy=False).tobytes())


def render(bundle: Path, output_dir: Path, metadata: Path | None = None,
           include_lfe: bool = False, lfe_gain_db: float = 0.0,
           max_samples: int | None = None) -> dict:
    infos, total = _load_bundle(bundle, max_samples)
    metadata = metadata or (bundle / "metadata.jsonl")
    events = _load_metadata(metadata, total)
    prepared_events = [_prepare_ramp_events(events[index], total)
                       for index in range(1, OBJECTS + 1)]
    prepared_positions = [[int(event["position"]) for event in prepared]
                          for prepared in prepared_events]
    lfe_gain = 10.0 ** (_finite(lfe_gain_db) / 20.0) if include_lfe else 0.0
    output_dir.mkdir(parents=True, exist_ok=True)
    wav_path = output_dir / ("object-direct-with-lfe.wav" if include_lfe else "object-direct-no-lfe.wav")
    stats = _Stats()
    object_stats = [_Stats() for _ in range(OBJECTS)]
    tail_blocks = []
    with wav_path.open("wb") as raw:
        # RIFF header is patched after the streaming render.
        raw.write(b"\0" * 44)
        for info in infos:
            _, audio, lfe = _read_batch(info["path"], info["start"])
            start = int(info["start"])
            count = int(info["samples"])
            weighted = np.empty_like(audio)
            for index in range(OBJECTS):
                weighted[index] = audio[index] * _gain_prepared_segment(
                    prepared_events[index], start, count, prepared_positions[index])
                object_stats[index].add(weighted[index].astype(np.float64))
            mono = weighted.sum(axis=0, dtype=np.float64)
            if include_lfe and len(lfe):
                mono += lfe.astype(np.float64) * lfe_gain
            stereo = np.column_stack((mono, mono)).astype(np.float32)
            if not np.isfinite(stereo).all():
                raise ValueError("non-finite direct stereo output")
            raw.write(stereo.astype("<f4", copy=False).tobytes())
            stats.add(mono)
            tail_blocks.append({"start": start, "end": start + count, "peak": float(np.max(np.abs(mono)))})
        payload_size = total * 2 * 4
        header = (b"RIFF" + struct.pack("<I", 36 + payload_size) + b"WAVE" +
                  b"fmt " + struct.pack("<IHHIIHH", 16, 3, 2, RATE, RATE * 8, 8, 32) +
                  b"data" + struct.pack("<I", payload_size))
        raw.seek(0)
        raw.write(header)
        result = {
        "schema": "audioplayer.object-direct.v1",
        "result": "PASS",
        "evidenceLayer": "renderer-neutral-object-pcm-sum",
        "notAClaim": ["not binaural", "not Windows Spatial endpoint output", "not subjective listening evidence"],
        "bundle": str(bundle.resolve()),
        "metadata": str(metadata.resolve()),
        "sampleRate": RATE,
        "objects": OBJECTS,
        "frames": total,
        "durationSeconds": total / RATE,
        "dynamicGainHeadroomDb": DYNAMIC_HEADROOM_DB,
        "dynamicVolume": DYNAMIC_VOLUME,
        "metadataSemantics": {
            "firstState": "immediate-snap",
            "laterState": "evaluate-previous-state-at-source-position-then-ramp",
            "overlap": "new-update-interrupts-previous-ramp",
            "samePosition": "last-per-object-state-wins",
            "gainRamp": "audible-to-audible interpolates gainDb then converts to amplitude; any silent endpoint interpolates amplitude",
        },
        "lfePolicy": "INCLUDED_EXPLICITLY" if include_lfe else "EXCLUDED_BY_DEFAULT",
        "lfeGainDb": lfe_gain_db if include_lfe else None,
        "lfeGainSemantics": "explicit linear gain on decoded LFE PCM; 0 dB is unity and is independent of dynamic-object -15 dB headroom",
        "outputWav": str(wav_path.resolve()),
        "output": stats.finish(),
        "objectsStats": [item.finish() for item in object_stats],
        "tail": {"lastBlock": tail_blocks[-1], "blockCount": len(tail_blocks),
                 "interpretation": "normal transport tail only; no isolated transient, so reverberation decay is not determined"},
    }
    report_path = output_dir / "object-direct-report.json"
    report_path.write_text(json.dumps(result, indent=2), encoding="utf-8")
    result["report"] = str(report_path.resolve())
    return result


class ObjectDirectTests(unittest.TestCase):
    def test_gain_and_ramp(self):
        events = [{"sourcePosition": 0, "rampDuration": 4, "active": True, "gainDb": 0.0},
                  {"sourcePosition": 4, "rampDuration": 4, "active": False, "gainDb": 0.0}]
        values = _gain_segment(events, 0, 8)
        self.assertTrue(np.allclose(values[:4], DYNAMIC_VOLUME))
        self.assertTrue(np.allclose(values[4:], [DYNAMIC_VOLUME,
                                                 DYNAMIC_VOLUME * .75,
                                                 DYNAMIC_VOLUME * .5,
                                                 DYNAMIC_VOLUME * .25]))

    def test_chunk_start_inside_ramp_and_overlap(self):
        events = [{"sourcePosition": 0, "rampDuration": 0, "active": True, "gainDb": 0.0},
                  {"sourcePosition": 4, "rampDuration": 8, "active": False, "gainDb": 0.0},
                  {"sourcePosition": 8, "rampDuration": 0, "active": True, "gainDb": -6.0}]
        full = _gain_segment(events, 0, 12)
        inside = _gain_segment(events, 6, 6)
        self.assertTrue(np.allclose(inside, full[6:]))
        # The update at 8 interrupts the preceding ramp at its evaluated
        # value; it must not restart from the old target or extend the ramp.
        self.assertAlmostEqual(float(full[8]), DYNAMIC_VOLUME * 10 ** (-6.0 / 20.0), places=7)

    def test_audible_to_audible_ramp_interpolates_db(self):
        events = [{"sourcePosition": 0, "rampDuration": 0, "active": True, "gainDb": 0.0},
                  {"sourcePosition": 4, "rampDuration": 4, "active": True, "gainDb": -6.0}]
        values = _gain_segment(events, 0, 8)
        self.assertAlmostEqual(float(values[4]), DYNAMIC_VOLUME, places=7)
        # dB midpoint is -3 dB, then the common -15 dB headroom is applied.
        self.assertAlmostEqual(float(values[6]), DYNAMIC_VOLUME * 10 ** (-3.0 / 20.0), places=7)
        self.assertAlmostEqual(float(values[7]), DYNAMIC_VOLUME * 10 ** (-4.5 / 20.0), places=7)

    def test_silent_to_audible_ramp_interpolates_amplitude(self):
        events = [{"sourcePosition": 0, "rampDuration": 0, "active": False, "gainDb": 0.0},
                  {"sourcePosition": 4, "rampDuration": 4, "active": True, "gainDb": 0.0}]
        values = _gain_segment(events, 0, 8)
        self.assertEqual(float(values[4]), 0.0)
        self.assertAlmostEqual(float(values[6]), DYNAMIC_VOLUME * 0.5, places=7)
        self.assertAlmostEqual(float(values[7]), DYNAMIC_VOLUME * 0.75, places=7)

    def test_duplicate_position_last_state_and_chunk_invariance(self):
        events = [{"sourcePosition": 0, "rampDuration": 0, "active": True, "gainDb": 0.0},
                  {"sourcePosition": 4, "rampDuration": 0, "active": False, "gainDb": 0.0},
                  {"sourcePosition": 4, "rampDuration": 0, "active": True, "gainDb": -3.0}]
        deduplicated = {}
        for event in events:
            deduplicated[int(event["sourcePosition"])] = event
        compact = [deduplicated[position] for position in sorted(deduplicated)]
        self.assertEqual(len(compact), 2)
        self.assertTrue(np.allclose(_gain_segment(compact, 0, 8)[4:],
                                    DYNAMIC_VOLUME * 10 ** (-3.0 / 20.0)))
        pieces = np.concatenate([_gain_segment(compact, 0, 3),
                                 _gain_segment(compact, 3, 5)])
        self.assertTrue(np.array_equal(pieces, _gain_segment(compact, 0, 8)))

    def test_rejects_nonfinite(self):
        with self.assertRaises(ValueError):
            _target_gain({"gainDb": float("nan")})

    def test_bundle_header_fixture(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "batch-00000000.bin"
            audio = np.zeros((OBJECTS, 4), dtype="<f4")
            path.write_bytes(b"BSCN" + struct.pack("<IIIqq", 2, OBJECTS, 4, 0, 4) +
                             audio.tobytes() + struct.pack("<I", 4) + b"\0" * 16)
            info, got_audio, got_lfe = _read_batch(path, 0)
            self.assertEqual(info["end"], 4)
            self.assertEqual(got_audio.shape, (OBJECTS, 4))
            self.assertEqual(len(got_lfe), 4)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bundle", type=Path)
    parser.add_argument("--metadata", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--include-lfe", action="store_true")
    parser.add_argument("--lfe-gain-db", type=float, default=0.0)
    parser.add_argument("--max-samples", type=int)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(ObjectDirectTests)
        result = unittest.TextTestRunner(verbosity=1).run(suite)
        raise SystemExit(0 if result.wasSuccessful() else 1)
    if args.bundle is None or args.output_dir is None:
        parser.error("--bundle and --output-dir are required unless --self-test is used")
    report = render(args.bundle, args.output_dir, args.metadata, args.include_lfe,
                    args.lfe_gain_db, args.max_samples)
    print(json.dumps({"result": report["result"], "report": report["report"],
                      "outputWav": report["outputWav"], "frames": report["frames"]}))


if __name__ == "__main__":
    main()
