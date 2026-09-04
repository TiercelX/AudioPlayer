"""Bounded L/R JOC slot contribution audit.

This diagnostic consumes an existing 5 s Gate6C bundle.  It never edits the
bundle; temporary one-slot bundles and BEAR renders are written below the
requested output directory.  ``--render-solo`` is deliberately opt-in because
it invokes the external BEAR reference renderer.
"""
from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np

RATE = 48000
FRAMES = 240000
FREQUENCIES = (173, 337, 521, 911, 1471, 2203, 3301, 4799, 6311, 7907, 9433, 11789)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(8 * 1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def read_bundles(root: Path) -> np.ndarray:
    chunks = []
    for path in sorted(root.glob("batch-*.bin")):
        with path.open("rb") as f:
            if f.read(4) != b"BSCN":
                raise ValueError(f"bad batch magic: {path}")
            version, objects, samples = struct.unpack("<III", f.read(12))
            start, end = struct.unpack("<qq", f.read(16))
            data = np.frombuffer(f.read(objects * samples * 4), dtype="<f4").reshape(objects, samples)
            lfe_n = struct.unpack("<I", f.read(4))[0]
            f.seek(lfe_n * 4, 1)
        if (version, objects) != (2, 15) or end != start + samples:
            raise ValueError(f"invalid batch header: {path.name}")
        chunks.append(data.copy())
    if not chunks:
        raise ValueError("empty bundle")
    return np.concatenate(chunks, axis=1)


def read_s24(path: Path, track: int) -> np.ndarray:
    raw = path.read_bytes()
    pos = 12
    fmt = data = None
    while pos + 8 <= len(raw):
        name, size = raw[pos:pos + 4], int.from_bytes(raw[pos + 4:pos + 8], "little")
        if name == b"fmt ": fmt = (pos + 8, size)
        if name == b"data": data = (pos + 8, size)
        pos += 8 + size + (size & 1)
    if fmt is None or data is None:
        raise ValueError("missing RIFF fmt/data")
    audio_format, channels, rate, _br, block, bits = struct.unpack("<HHIIHH", raw[fmt[0]:fmt[0] + 16])
    if (audio_format, channels, rate, block, bits) != (1, 18, RATE, 54, 24):
        raise ValueError("expected 18ch/48k/S24 source")
    frames = min(FRAMES, data[1] // block)
    out = np.empty(frames, dtype=np.float64)
    base = data[0] + (track - 1) * 3
    for i in range(frames):
        q = int.from_bytes(raw[base + i * block:base + i * block + 3], "little", signed=False)
        out[i] = ((q ^ 0x800000) - 0x800000) / 8388608.0
    return out


def response(x: np.ndarray, hz: float) -> complex:
    lo, hi = int(.5 * RATE), int(4.5 * RATE)
    t = np.arange(hi - lo, dtype=np.float64) / RATE
    return 2.0 * np.dot(x[lo:hi], np.exp(-2j * np.pi * hz * t)) / (hi - lo)


def response_segment(x: np.ndarray, start: int, hz: float) -> complex:
    t = (start + np.arange(len(x), dtype=np.float64)) / RATE
    return 2.0 * np.dot(x, np.exp(-2j * np.pi * hz * t)) / max(len(x), 1)


def load_metadata(bundle: Path, at_source_position: int = int(.5 * RATE)) -> dict[int, dict]:
    by_object = {}
    for line in (bundle / "metadata.jsonl").read_text(encoding="utf-8").splitlines():
        for update in json.loads(line).get("updates", []):
            oi = int(update["objectIndex"])
            if int(update.get("sourcePosition", 0)) <= at_source_position:
                by_object[oi] = update
    if len(by_object) != 15:
        raise ValueError(f"expected 15 metadata objects at {at_source_position}, found {len(by_object)}")
    return by_object


def polar(update: dict) -> tuple[float, float, float]:
    # Same pinned EAR/BS.2127 mapping used by the checked-in renderer.
    sys.path.insert(0, str(Path(__file__).parents[1] / "tools" / "atmos-render"))
    from run_bear_montero_bundle import _ear_point_cart_to_polar
    return tuple(float(v) for v in _ear_point_cart_to_polar(
        2 * float(update.get("standardX", .5)) - 1,
        1 - 2 * float(update.get("standardY", .5)),
        float(update.get("standardZ", 0))))


def copy_one_hot(bundle: Path, out: Path, slot: int, positions: dict[int, tuple[float, float, float]] | None = None) -> None:
    out.mkdir(parents=True, exist_ok=False)
    for path in sorted(bundle.glob("batch-*.bin")):
        with path.open("rb") as f:
            header = f.read(4 + 12 + 16)
            version, objects, samples = struct.unpack("<III", header[4:16])
            audio = np.frombuffer(f.read(objects * samples * 4), dtype="<f4").reshape(objects, samples).copy()
            tail = f.read()
        audio[np.arange(objects) != slot - 1, :] = 0
        (out / path.name).write_bytes(header + audio.astype("<f4").tobytes() + tail)
    metadata = []
    for line in (bundle / "metadata.jsonl").read_text(encoding="utf-8").splitlines():
        row = json.loads(line)
        if positions:
            for update in row.get("updates", []):
                replacement = positions.get(int(update["objectIndex"]))
                if replacement:
                    update["standardX"], update["standardY"], update["standardZ"] = replacement
        metadata.append(json.dumps(row, separators=(",", ":")))
    (out / "metadata.jsonl").write_text("\n".join(metadata) + "\n", encoding="utf-8")
    shutil.copy2(bundle / "bundle.complete", out / "bundle.complete")


def metrics(source: np.ndarray, candidate: np.ndarray) -> dict:
    gains = []
    for hz in FREQUENCIES:
        den = response(source, hz)
        gains.append([[float((response(candidate[:, c], hz) / den).real),
                       float((response(candidate[:, c], hz) / den).imag)] for c in range(2)])
    return {"rms": [float(np.sqrt(np.mean(candidate[:, c] ** 2))) for c in range(2)],
            "peak": [float(np.max(np.abs(candidate[:, c]))) for c in range(2)],
            "complexGain": gains}


def read_f32(path: Path) -> np.ndarray:
    raw = path.read_bytes()
    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise ValueError(f"not RIFF/WAVE: {path}")
    if len(raw) < 12 or int.from_bytes(raw[4:8], "little") + 8 > len(raw):
        raise ValueError(f"truncated RIFF: {path}")
    pos = 12
    fmt = data = None
    while pos < len(raw):
        if pos + 8 > len(raw):
            raise ValueError(f"truncated RIFF chunk header: {path}")
        name, size = raw[pos:pos + 4], int.from_bytes(raw[pos + 4:pos + 8], "little")
        payload = pos + 8
        end = payload + size
        if end > len(raw):
            raise ValueError(f"truncated RIFF chunk: {path}")
        if name == b"fmt ":
            if fmt is not None or size < 16:
                raise ValueError(f"invalid/duplicate fmt chunk: {path}")
            fmt = (payload, size)
        elif name == b"data":
            if data is not None:
                raise ValueError(f"duplicate data chunk: {path}")
            data = (payload, size)
        pos = end + (size & 1)
        if pos > len(raw):
            raise ValueError(f"truncated RIFF padding: {path}")
    if fmt is None or data is None:
        raise ValueError(f"missing data chunk: {path}")
    audio_format, channels, rate, _br, block, bits = struct.unpack("<HHIIHH", raw[fmt[0]:fmt[0] + 16])
    if (audio_format, channels, rate, block, bits) != (3, 2, RATE, 8, 32) or data[1] % 8:
        raise ValueError(f"unexpected stereo float RIFF format: {path}")
    values = np.frombuffer(raw[data[0]:data[0] + data[1]], dtype="<f4")
    if not np.isfinite(values).all():
        raise ValueError(f"nonfinite stereo float RIFF: {path}")
    return values.reshape(-1, 2).astype(np.float64)


def endpoint_score(stimulus: np.ndarray, reference: np.ndarray, candidate: np.ndarray) -> dict:
    ild, ipd, error = [], [], 0.0
    for hz in FREQUENCIES:
        den = response(stimulus, hz)
        ref = np.array([response(reference[:, c], hz) / den for c in range(2)])
        got = np.array([response(candidate[:, c], hz) / den for c in range(2)])
        error += float(np.sum(np.abs(got - ref) ** 2))
        got_ild = 20 * np.log10(max(abs(got[0]), 1e-30) / max(abs(got[1]), 1e-30))
        ref_ild = 20 * np.log10(max(abs(ref[0]), 1e-30) / max(abs(ref[1]), 1e-30))
        ild.append(float(got_ild - ref_ild))
        ipd.append(float(np.angle(np.exp(1j * (np.angle(got[0] / got[1]) - np.angle(ref[0] / ref[1]))))))
    return {"complexError": error, "maxAbsDeltaIldDb": max(abs(v) for v in ild),
            "maxAbsDeltaIpdRad": max(abs(v) for v in ipd), "deltaIldDb": ild, "deltaIpdRad": ipd}


def endpoint_score_gains(reference_gains: np.ndarray, candidate_gains: np.ndarray) -> dict:
    ild, ipd, error = [], [], 0.0
    for ref, got in zip(reference_gains, candidate_gains):
        error += float(np.sum(np.abs(got - ref) ** 2))
        got_ild = 20 * np.log10(max(abs(got[0]), 1e-30) / max(abs(got[1]), 1e-30))
        ref_ild = 20 * np.log10(max(abs(ref[0]), 1e-30) / max(abs(ref[1]), 1e-30))
        ild.append(float(got_ild - ref_ild))
        ipd.append(float(np.angle(np.exp(1j * (np.angle(got[0] / got[1]) - np.angle(ref[0] / ref[1]))))))
    return {"complexError": error, "maxAbsDeltaIldDb": max(abs(v) for v in ild),
            "maxAbsDeltaIpdRad": max(abs(v) for v in ipd), "deltaIldDb": ild, "deltaIpdRad": ipd}


def self_test() -> None:
    assert len(FREQUENCIES) == 12
    assert np.allclose(np.sum(np.array([[1., 2.], [3., 4.]]), axis=0), [4., 6.])
    assert tuple(round(x, 6) for x in polar({"standardX": 0, "standardY": 0, "standardZ": 0})) == (30.0, 0.0, 1.0)
    with __import__("tempfile").TemporaryDirectory() as d:
        root = Path(d) / "source"; root.mkdir()
        pcm = np.arange(30, dtype="<f4").reshape(15, 2)
        batch = b"BSCN" + struct.pack("<IIIqq", 2, 15, 2, 0, 2) + pcm.tobytes() + struct.pack("<I", 0)
        (root / "batch-00000000.bin").write_bytes(batch)
        updates = [{"objectIndex": i, "standardX": i / 10, "standardY": 0.0, "standardZ": 0.0, "sourcePosition": 0} for i in range(1, 16)]
        (root / "metadata.jsonl").write_text(json.dumps({"updates": updates}) + "\n", encoding="utf-8")
        (root / "bundle.complete").write_text("complete\n", encoding="utf-8")
        before = {p.name: sha256(p) for p in root.iterdir() if p.is_file()}
        one = Path(d) / "one"; copy_one_hot(root, one, 3, {3: (.25, .5, .75)})
        with (one / "batch-00000000.bin").open("rb") as stream:
            stream.seek(32); result = np.frombuffer(stream.read(15 * 2 * 4), dtype="<f4").reshape(15, 2)
        assert np.array_equal(result[2], pcm[2]) and np.count_nonzero(result[np.arange(15) != 2]) == 0
        rewritten = json.loads((one / "metadata.jsonl").read_text(encoding="utf-8"))["updates"]
        assert next(u for u in rewritten if u["objectIndex"] == 3)["standardX"] == .25
        assert next(u for u in rewritten if u["objectIndex"] == 2)["standardX"] == .2
        assert before == {p.name: sha256(p) for p in root.iterdir() if p.is_file()}
        valid = b"fmt " + struct.pack("<I", 16) + struct.pack("<HHIIHH", 3, 2, RATE, RATE * 8, 8, 32)
        payload = np.zeros((2, 2), dtype="<f4").tobytes()
        good = Path(d) / "good.wav"; good.write_bytes(b"RIFF" + struct.pack("<I", 4 + len(valid) + 8 + len(payload)) + b"WAVE" + valid + b"data" + struct.pack("<I", len(payload)) + payload)
        assert read_f32(good).shape == (2, 2)
        for bad_data in (b"RIFF\x00\x00\x00\x00WAVEfmt ", good.read_bytes()[:-1], good.read_bytes().replace(b"data\x10\x00\x00\x00", b"data\xff\xff\xff\x7f")):
            bad = Path(d) / f"bad-{len(bad_data)}.wav"; bad.write_bytes(bad_data)
            try:
                read_f32(bad)
            except ValueError:
                pass
            else:
                raise AssertionError("malformed/truncated RIFF was accepted")
    print("lrJocContributionAuditSelfTest=PASS frequencies=12 oneHot=1 position=1")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--summary", type=Path, default=Path("tmp/oracle/single-object-oracle-bounded-5s-r4/single-object-oracle-summary.json"))
    ap.add_argument("--output", type=Path, default=Path("tmp/oracle/lr-slot-audit-r1"))
    ap.add_argument("--render-solo", action="store_true")
    ap.add_argument("--permutation-search", action="store_true",
                    help="also render each active PCM slot at each active metadata position")
    ap.add_argument("--reuse-render-root", type=Path,
                    help="reuse previously rendered solo/permutation WAVs below this root")
    ap.add_argument("--python-exe", default=sys.executable)
    ap.add_argument("--bear-python")
    args = ap.parse_args()
    if args.self_test:
        self_test(); return
    if args.render_solo and not args.bear_python and not args.reuse_render_root:
        ap.error("--bear-python is required with --render-solo")
    summary = json.loads(args.summary.read_text(encoding="utf-8"))
    reports = []
    permutation_cases = []
    for case in summary["caseResults"]:
        if case["sourceAdmObjectName"] not in ("L", "R"):
            continue
        name = case["caseName"]
        bundle = Path(next(x["bundle"] for x in summary["native"] if x["case"] == name))
        # Endpoint reports are keyed by case in older revisions; use the slot
        # report provenance, which is stable across all bounded revisions.
        slot_report = Path("tmp/oracle/single-object-oracle-bounded-5s-r4/slot-reports-rerun") / f"{name}.json"
        source = Path(json.loads(slot_report.read_text(encoding="utf-8"))["source"])
        audio = read_bundles(bundle)
        selected_track = case["selectedObjectTrackOneBased"]
        stimulus = read_s24(source, selected_track)
        stable_energy = np.sum(audio[:, int(.5 * RATE):int(4.5 * RATE)].astype(np.float64) ** 2, axis=1)
        order = [int(index + 1) for index in np.argsort(stable_energy)[::-1] if stable_energy[index] > 1e-16]
        meta = load_metadata(bundle)
        trace_path = Path("tmp/oracle/oamd-counterfactual-r4") / name / "au-slot-trace.json"
        trace_doc = json.loads(trace_path.read_text(encoding="utf-8"))
        denominators = [response(stimulus, hz) for hz in FREQUENCIES]
        au_trace = []
        for au in trace_doc["trace"]:
            lo, hi = int(au["batchStart"]), min(int(au["batchEnd"]), audio.shape[1])
            entries = []
            for index, slot in enumerate(au["slots"]):
                gains = [response_segment(audio[index, lo:hi], lo, hz) / den for hz, den in zip(FREQUENCIES, denominators)] if hi > lo else [0j] * len(FREQUENCIES)
                entries.append({"slot": index + 1, "objectIndex": slot["objectIndex"],
                                "pcmRms": slot["pcmRms"], "pcmEnergy": slot["pcmEnergy"],
                                "active": slot["active"], "gainDb": slot["gainDb"],
                                "standardXYZ": [slot["standardX"], slot["standardY"], slot["standardZ"]],
                                "officialBearPolar": list(polar(slot)),
                                "perFrequencyComplexGain": [[float(v.real), float(v.imag)] for v in gains]})
            au_trace.append({"au": au["au"], "sourcePosition": au["sourcePosition"],
                             "batchStart": lo, "batchEnd": hi, "slots": entries})
        slots = []
        for slot_id in order:
            index = slot_id - 1
            g = [response(audio[index], hz) / response(stimulus, hz) for hz in FREQUENCIES]
            slots.append({"slot": slot_id, "energyShare": float(stable_energy[index] / max(np.sum(stable_energy), 1e-30)),
                          "pcmRms": float(np.sqrt(np.mean(audio[index, int(.5 * RATE):int(4.5 * RATE)] ** 2))),
                          "oamdObjectIndex": slot_id, "standardXYZ": [meta[slot_id].get(k) for k in ("standardX", "standardY", "standardZ")],
                          "officialBearPolar": list(polar(meta[slot_id])),
                          "perFrequencyComplexGain": [[float(v.real), float(v.imag)] for v in g]})
        report = {"case": name, "object": case["sourceAdmObjectName"], "selectedTrack": selected_track,
                  "bundleSha256": sha256(bundle / "metadata.jsonl"), "sourceSha256": sha256(source),
                  "normalizationApplied": False, "stableWindow": [24000, 216000],
                  "auCount": len(au_trace), "steadyStateSourcePosition": 7680,
                  "auSlotTraceSource": str(trace_path.resolve()), "slots": slots,
                  "renderedSolo": []}
        if args.render_solo:
            render_case_root = (args.reuse_render_root / name) if args.reuse_render_root else (args.output / name)
            solo_root = render_case_root / "solo"; solo_root.mkdir(parents=True, exist_ok=True)
            cmd_base = [args.python_exe, str(Path(__file__).with_name("render_bounded_joc_bundle.py")),
                        "--bear-python", "tmp/reference/bear-git", "--data", "docs/dev/reference-cache/bear-default.tf"]
            for slot in order:
                one = solo_root / f"slot-{slot:02d}" / "bundle"
                out = solo_root / f"slot-{slot:02d}" / "render"
                raw = out / "MONTERO-BEAR-open-reference-bounded-5s-raw-f32.wav"
                if args.reuse_render_root:
                    one = render_case_root / "solo" / f"slot-{slot:02d}" / "bundle"
                    out = render_case_root / "solo" / f"slot-{slot:02d}" / "render"
                    raw = out / "MONTERO-BEAR-open-reference-bounded-5s-raw-f32.wav"
                else:
                    copy_one_hot(bundle, one, slot)
                    out.mkdir()
                    cmd = cmd_base + [str(one), "--output-dir", str(out), "--source-input", str(source)]
                    cmd += ["--python-path", "tmp/reference/ear-2.1.0/ebu_adm_renderer-2.1.0", "--python-path", "tmp/reference/bear-git", "--python-path", "tmp/reference/VISR-install/python", "--python-path", "tmp/reference/bear-git-build-shared/python/Release", "--python-path", "tools/atmos-render"]
                    cmd += ["--dll-dir", "tmp/reference/bear-install/bin", "--dll-dir", "tmp/reference/VISR-install/lib", "--dll-dir", "tmp/reference/VISR-install/3rd", "--dll-dir", "tmp/reference/VISR-0.13.0-build-shared/lib/Release", "--dll-dir", "tmp/reference/bear-git-build-shared/python/Release"]
                    with (out / "render.log").open("w", encoding="utf-8") as log:
                        subprocess.run(cmd, check=True, cwd=Path(__file__).parents[1], stdout=log, stderr=subprocess.STDOUT)
                report["renderedSolo"].append({"slot": slot, "path": str(raw.resolve()), "metrics": metrics(stimulus, read_f32(raw))})
            full_path = Path("tmp/oracle/oamd-fixed-ear-r2") / name / "MONTERO-BEAR-open-reference-bounded-5s-raw-f32.wav"
            solo_audio = [read_f32(Path(item["path"])) for item in report["renderedSolo"]]
            summed = np.sum(solo_audio, axis=0)
            full = read_f32(full_path)
            diff = summed - full
            report["fullRender"] = str(full_path.resolve())
            report["soloSumVsFull"] = {"soloSlots": [item["slot"] for item in report["renderedSolo"]],
                                        "maxAbs": float(np.max(np.abs(diff))),
                                        "rms": float(np.sqrt(np.mean(diff * diff))),
                                        "fullRms": float(np.sqrt(np.mean(full * full))),
                                        "relativeRms": float(np.sqrt(np.mean(diff * diff)) / max(np.sqrt(np.mean(full * full)), 1e-30)),
                                        "normalizationApplied": False}
            if args.permutation_search:
                perm_root = render_case_root / "permutation"
                perm_root.mkdir(parents=True, exist_ok=True)
                active = order
                rendered = {}
                cmd_base = [args.python_exe, str(Path(__file__).with_name("render_bounded_joc_bundle.py")),
                            "--bear-python", "tmp/reference/bear-git", "--data", "docs/dev/reference-cache/bear-default.tf"]
                fixed_args = ["--python-path", "tmp/reference/ear-2.1.0/ebu_adm_renderer-2.1.0", "--python-path", "tmp/reference/bear-git", "--python-path", "tmp/reference/VISR-install/python", "--python-path", "tmp/reference/bear-git-build-shared/python/Release", "--python-path", "tools/atmos-render", "--dll-dir", "tmp/reference/bear-install/bin", "--dll-dir", "tmp/reference/VISR-install/lib", "--dll-dir", "tmp/reference/VISR-install/3rd", "--dll-dir", "tmp/reference/VISR-0.13.0-build-shared/lib/Release", "--dll-dir", "tmp/reference/bear-git-build-shared/python/Release"]
                positions = {i: tuple(meta[i].get(k) for k in ("standardX", "standardY", "standardZ")) for i in active}
                for src_slot in active:
                    for dst_slot in active:
                        tag = f"slot-{src_slot:02d}-at-{dst_slot:02d}"
                        one = perm_root / tag / "bundle"
                        out = perm_root / tag / "render"
                        if not args.reuse_render_root:
                            copy_one_hot(bundle, one, src_slot, {src_slot: positions[dst_slot]})
                            out.mkdir()
                            cmd = cmd_base + [str(one), "--output-dir", str(out), "--source-input", str(source)] + fixed_args
                            with (out / "render.log").open("w", encoding="utf-8") as log:
                                subprocess.run(cmd, check=True, cwd=Path(__file__).parents[1], stdout=log, stderr=subprocess.STDOUT)
                        rendered[(src_slot, dst_slot)] = read_f32(out / "MONTERO-BEAR-open-reference-bounded-5s-raw-f32.wav")
                source_ref = read_f32(Path("tmp/oracle/single-object-oracle-bounded-5s-r4/source-bear") / name / "source-object-only-raw-f32.wav")
                denominators = np.array([response(stimulus, hz) for hz in FREQUENCIES])
                reference_gains = np.array([[response(source_ref[:, c], hz) / denominators[j] for c in range(2)] for j, hz in enumerate(FREQUENCIES)])
                rendered_gains = {(src, dst): np.array([[response(wave[:, c], hz) / denominators[j] for c in range(2)] for j, hz in enumerate(FREQUENCIES)]) for (src, dst), wave in rendered.items()}
                scores = []
                for perm in itertools.permutations(active):
                    candidate_gains = np.sum([rendered_gains[(src, dst)] for src, dst in zip(active, perm)], axis=0)
                    scores.append({"mappingSlotToDestination": dict(zip(active, perm)), "score": endpoint_score_gains(reference_gains, candidate_gains)})
                scores.sort(key=lambda item: item["score"]["complexError"])
                report["permutationSearch"] = {"activeSlots": active, "candidateCount": len(scores), "baseline": next(item for item in scores if item["mappingSlotToDestination"] == dict(zip(active, active))), "best": scores[0], "top5": scores[:5], "normalizationApplied": False}
                permutation_cases.append((name, active, rendered_gains, reference_gains))
        reports.append(report)
    args.output.mkdir(parents=True, exist_ok=True)
    root_report = {"schema": 1, "reports": reports}
    if len(permutation_cases) >= 2 and all(tuple(sorted(item[1])) == tuple(sorted(permutation_cases[0][1])) for item in permutation_cases):
        active = permutation_cases[0][1]
        common_scores = []
        for perm in itertools.permutations(active):
            mapping = dict(zip(active, perm)); total = 0.0; case_scores = {}
            for name, _slots, gains, ref in permutation_cases:
                candidate = np.sum([gains[(src, dst)] for src, dst in mapping.items()], axis=0)
                score = endpoint_score_gains(ref, candidate); total += score["complexError"]; case_scores[name] = score
            common_scores.append({"mappingSlotToDestination": mapping, "complexError": total, "caseScores": case_scores})
        common_scores.sort(key=lambda item: item["complexError"])
        baseline_mapping = dict(zip(active, active))
        root_report["commonPermutationSearch"] = {"activeSlots": active, "candidateCount": len(common_scores),
            "baseline": next(item for item in common_scores if item["mappingSlotToDestination"] == baseline_mapping),
            "best": common_scores[0], "top5": common_scores[:5], "normalizationApplied": False}
    (args.output / "lr-slot-contribution-summary.json").write_text(json.dumps(root_report, indent=2) + "\n", encoding="utf-8")
    print(f"lrJocContributionAudit=PASS cases={len(reports)} output={args.output.resolve()}")


if __name__ == "__main__":
    main()
