"""Audit JOC PCM and OAMD semantics with bounded BEAR counterfactuals.

The existing r4 bundle is rendered as A.  B keeps its decoded PCM, gain, and
timing but forces every slot to the source ADM object's position.  C forces
every slot to the front midpoint. D forces every slot to the exact polar
position emitted by the pinned official EAR ``convert_objects`` adapter. E
preserves each slot's OAMD position/timing and replaces only the project
Cartesian-to-polar function with the pinned EAR conversion. These are diagnostic
counterfactuals, not
normative ADM or Dolby rendering claims.  Inputs and outputs are never changed
in place; reuse requires a provenance marker containing bundle/data hashes and
the exact forced position.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import struct
import sys
import tempfile
from pathlib import Path

import numpy as np
from scipy.signal import correlate, correlation_lags


RATE = 48_000
FRAMES = 240_000
FREQUENCIES = (173, 337, 521, 911, 1471, 2203, 3301, 4799, 6311, 7907, 9433, 11789)
MODES = ("A", "B", "C", "D", "E")
EXPECTED_OFFICIAL_POLAR = ((30.0, 0.0, 1.0), (-30.0, 0.0, 1.0), (0.0, 0.0, 1.0),
                          (70.0, 0.0, 1.0), (-70.0, 0.0, 1.0), (110.0, 0.0, 1.0),
                          (-110.0, 0.0, 1.0), (0.0, 90.0, 1.0))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def bundle_sha256(bundle: Path) -> str:
    digest = hashlib.sha256()
    for path in sorted(bundle.glob("batch-*.bin")) + [bundle / "metadata.jsonl"]:
        digest.update(path.name.encode("utf-8"))
        digest.update(sha256(path).encode("ascii"))
    return digest.hexdigest()


def chunks(path: Path):
    with path.open("rb") as stream:
        if stream.read(4) != b"RIFF":
            raise ValueError("source must be RIFF/WAVE")
        stream.read(4)
        if stream.read(4) != b"WAVE":
            raise ValueError("source must be RIFF/WAVE")
        while True:
            header = stream.read(8)
            if not header:
                return
            if len(header) != 8:
                raise ValueError("truncated WAV chunk header")
            name = header[:4].decode("latin1")
            size = int.from_bytes(header[4:], "little")
            offset = stream.tell()
            yield name, size, offset
            stream.seek(size + (size & 1), 1)


def read_s24_object(path: Path, object_track_one_based: int) -> np.ndarray:
    table = {name: (size, offset) for name, size, offset in chunks(path)}
    fmt_size, fmt_offset = table["fmt "]
    data_size, data_offset = table["data"]
    with path.open("rb") as stream:
        stream.seek(fmt_offset)
        audio_format, channels, rate, _br, block, bits = __import__("struct").unpack(
            "<HHIIHH", stream.read(fmt_size))
        if (audio_format, channels, rate, block, bits) != (1, 18, RATE, 54, 24):
            raise ValueError("counterfactual input must be 18ch/48k S24 RIFF")
        frames = min(FRAMES, data_size // block)
        stream.seek(data_offset + (object_track_one_based - 1) * 3)
        raw = bytearray()
        for _ in range(frames):
            raw.extend(stream.read(3))
            stream.seek(block - 3, 1)
    packed = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
    values = packed[:, 0].astype(np.int32) | (packed[:, 1].astype(np.int32) << 8) | (packed[:, 2].astype(np.int32) << 16)
    return ((values ^ 0x800000) - 0x800000).astype(np.float64) / 8_388_608.0


def read_f32_stereo(path: Path) -> np.ndarray:
    table = {name: (size, offset) for name, size, offset in chunks(path)}
    fmt_size, fmt_offset = table["fmt "]
    data_size, data_offset = table["data"]
    with path.open("rb") as stream:
        stream.seek(fmt_offset)
        audio_format, channels, rate, _br, block, bits = struct.unpack("<HHIIHH", stream.read(fmt_size))
        if (audio_format, channels, rate, block, bits) != (3, 2, RATE, 8, 32):
            raise ValueError(f"unexpected stereo float WAV: {path}")
        frames = min(FRAMES, data_size // block)
        stream.seek(data_offset)
        data = np.frombuffer(stream.read(frames * block), dtype="<f4")
    if data.size != frames * 2:
        raise ValueError(f"short stereo float WAV: {path}")
    return data.reshape(frames, 2).astype(np.float64)


def read_trace(bundle: Path, output: Path) -> dict:
    metadata = [json.loads(line) for line in (bundle / "metadata.jsonl").read_text(encoding="utf-8").splitlines() if line]
    trace = []
    last_updates = None
    for row in metadata:
        batch = int(row["batch"])
        path = bundle / f"batch-{batch:08d}.bin"
        with path.open("rb") as stream:
            if stream.read(4) != b"BSCN":
                raise ValueError(f"bad batch magic: {path.name}")
            version = int.from_bytes(stream.read(4), "little")
            objects = int.from_bytes(stream.read(4), "little")
            samples = int.from_bytes(stream.read(4), "little")
            start = int.from_bytes(stream.read(8), "little", signed=True)
            end = int.from_bytes(stream.read(8), "little", signed=True)
            pcm = np.frombuffer(stream.read(objects * samples * 4), dtype="<f4").reshape(objects, samples)
        if version != 2 or objects != 15 or end != start + samples or not np.isfinite(pcm).all():
            raise ValueError(f"invalid batch contract: {path.name}")
        updates = sorted(row.get("updates", []), key=lambda update: int(update["objectIndex"]))
        metadata_count = len(updates)
        metadata_source_position = (int(updates[0]["sourcePosition"]) if updates and
                                    updates[0].get("sourcePosition") is not None else None)
        if not updates and last_updates is not None:
            updates = last_updates
        if len(updates) != 15:
            raise ValueError(f"metadata count is not 15 at AU {batch}")
        last_updates = updates
        slots = []
        for index, update in enumerate(updates):
            slots.append({
                "objectIndex": int(update["objectIndex"]),
                "standardX": update.get("standardX"), "standardY": update.get("standardY"),
                "standardZ": update.get("standardZ"), "active": bool(update.get("active")),
                "gainDb": update.get("gainDb"), "gainMinusInfinity": bool(update.get("gainMinusInfinity")),
                "rampDuration": int(update.get("rampDuration", 0)),
                "pcmRms": float(np.sqrt(np.mean(pcm[index].astype(np.float64) ** 2))),
                "pcmEnergy": float(np.sum(pcm[index].astype(np.float64) ** 2)),
            })
        trace.append({"au": batch, "sourcePosition": metadata_source_position if metadata_source_position is not None else int(updates[0]["sourcePosition"]),
                      "metadataSourcePosition": metadata_source_position,
                      "outputStart": int(row.get("outputStart", start)),
                      "outputEnd": int(row.get("outputEnd", end)),
                      "metadataCount": metadata_count, "batchStart": start, "batchEnd": end,
                      "samples": samples, "slots": slots})
    result = {"bundle": str(bundle.resolve()), "bundleSha256": bundle_sha256(bundle),
              "accessUnits": len(trace), "steadyStateSourcePosition": 7680, "slotsPerAu": 15,
              "trace": trace}
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def response(samples: np.ndarray, frequency: float) -> complex:
    start, end = int(.5 * RATE), int(4.5 * RATE)
    t = np.arange(end - start, dtype=np.float64) / RATE
    return 2.0 * np.dot(samples[start:end], np.exp(-2j * np.pi * frequency * t)) / len(t)


def wideband(candidate: np.ndarray, reference: np.ndarray) -> dict:
    start, end = int(.5 * RATE), int(4.5 * RATE)
    y, x = candidate[start:end], reference[start:end]
    y0, x0 = y - np.mean(y), x - np.mean(x)
    cross = correlate(y0, x0, mode="full", method="fft")
    lags = correlation_lags(len(y0), len(x0), mode="full")
    index = int(np.argmax(np.abs(cross)))
    scalar = float(np.dot(y, x) / max(np.dot(x, x), 1e-30))
    residual = y - scalar * x
    return {"correlation": float(cross[index] / np.sqrt(np.dot(y0, y0) * np.dot(x0, x0))),
            "lagSamples": int(lags[index]), "bestScalarGain": scalar,
            "residualRms": float(np.sqrt(np.mean(residual * residual))),
            "residualEnergy": float(np.sum(residual * residual))}


def compare(source_object: np.ndarray, source: np.ndarray, candidate: np.ndarray, mode: str) -> dict:
    rows = []
    for frequency in FREQUENCIES:
        source_gain = [response(source[:, channel], frequency) / response(source_object, frequency) for channel in range(2)]
        candidate_gain = [response(candidate[:, channel], frequency) / response(source_object, frequency) for channel in range(2)]
        source_ild = 20 * np.log10(max(abs(source_gain[0]), 1e-30) / max(abs(source_gain[1]), 1e-30))
        candidate_ild = 20 * np.log10(max(abs(candidate_gain[0]), 1e-30) / max(abs(candidate_gain[1]), 1e-30))
        source_ipd = np.angle(source_gain[0] / source_gain[1])
        candidate_ipd = np.angle(candidate_gain[0] / candidate_gain[1])
        rows.append({"frequencyHz": frequency, "sourceComplexGain": [[float(g.real), float(g.imag)] for g in source_gain],
                     "candidateComplexGain": [[float(g.real), float(g.imag)] for g in candidate_gain],
                     "deltaIldDb": float(candidate_ild - source_ild),
                     "deltaIpdRad": float(np.angle(np.exp(1j * (candidate_ipd - source_ipd))))})
    return {"mode": mode, "framesCompared": min(len(source), len(candidate)), "normalizationApplied": False,
            "peak": [float(np.max(np.abs(candidate[:, c]))) for c in range(2)],
            "rms": [float(np.sqrt(np.mean(candidate[:, c] ** 2))) for c in range(2)],
            "peakRatioToSource": [float(np.max(np.abs(candidate[:, c])) / max(np.max(np.abs(source[:, c])), 1e-30)) for c in range(2)],
            "rmsRatioToSource": [float(np.sqrt(np.mean(candidate[:, c] ** 2)) / max(np.sqrt(np.mean(source[:, c] ** 2)), 1e-30)) for c in range(2)],
            "wideband": {"left": wideband(candidate[:, 0], source[:, 0]), "right": wideband(candidate[:, 1], source[:, 1])},
            "perFrequency": rows}


def standard_position(cartesian: dict) -> tuple[float, float, float]:
    return ((float(cartesian["x"]) + 1.0) / 2.0, (1.0 - float(cartesian["y"])) / 2.0, float(cartesian["z"]))


def bear_polar(standard: tuple[float, float, float]) -> tuple[float, float, float]:
    """Mirror the existing adapter's ETSI-standard-XYZ to BEAR conversion."""
    x = 2.0 * standard[0] - 1.0
    y = 1.0 - 2.0 * standard[1]
    z = standard[2]
    distance = float(np.sqrt(x * x + y * y + z * z))
    if distance < 1e-9:
        return (0.0, 0.0, 1.0)
    return (float(np.degrees(np.arctan2(-x, y))),
            float(np.degrees(np.arcsin(np.clip(z / distance, -1.0, 1.0)))),
            min(1.0, max(.1, distance)))


def official_bear_positions(args, source: Path) -> list[dict]:
    """Capture the pinned EAR/BEAR ADM-to-ObjectsInput conversion."""
    for directory in args.dll_dir:
        path = Path(directory).resolve()
        if hasattr(os, "add_dll_directory") and path.is_dir():
            os.add_dll_directory(str(path))
    for directory in [args.bear_python, *args.python_path]:
        resolved = str(Path(directory).resolve())
        if resolved not in sys.path:
            sys.path.insert(0, resolved)
    from bear.render_cli import BEAROfflineRenderDriver, convert_objects
    from ear.fileio import openBw64Adm
    from ear.core.metadata_input import ObjectRenderingItem

    driver = BEAROfflineRenderDriver(input_file=str(source), output_file=str(source),
                                     bear_data="file:" + str(Path(args.data).resolve()),
                                     fft_implementation="default", conversion_mode="to_polar")
    with openBw64Adm(str(source)) as infile:
        infile.adm.validate()
        objects = [item for item in driver.get_rendering_items(infile.adm)
                   if isinstance(item, ObjectRenderingItem)]
        if len(objects) != 8:
            raise RuntimeError(f"official EAR source adapter returned {len(objects)} objects")
        positions = []
        for index, item in enumerate(objects):
            converted = convert_objects(item.metadata_source.get_next_block(), index)
            position = converted.type_metadata.position
            observed = (float(position.azimuth), float(position.elevation), float(position.distance))
            expected = EXPECTED_OFFICIAL_POLAR[index]
            if not all(abs(actual - wanted) < 1e-6 for actual, wanted in zip(observed, expected)):
                raise RuntimeError(f"official EAR position changed for object {index + 1}: {observed} != {expected}")
            positions.append({"objectIndex": index + 1, "azimuth": observed[0],
                              "elevation": observed[1], "distance": observed[2]})
    return positions


def expected_marker(bundle: Path, data: Path, mode: str,
                    forced_standard: tuple[float, float, float] | None,
                    forced_polar: tuple[float, float, float] | None,
                    official_conversion: bool = False) -> dict:
    return {"schema": 1, "mode": mode, "bundleSha256": bundle_sha256(bundle), "bearDataSha256": sha256(data),
            "forcedStandardPosition": list(forced_standard) if forced_standard is not None else None,
            "forcedPolarPosition": list(forced_polar) if forced_polar is not None else None,
            "officialEarPolarConversion": official_conversion}


def render_counterfactual(args, bundle: Path, data: Path, out: Path, mode: str,
                          forced_standard: tuple[float, float, float] | None,
                          forced_polar: tuple[float, float, float] | None,
                          official_conversion: bool = False) -> Path:
    marker = out / "counterfactual-provenance.json"
    expected = expected_marker(bundle, data, mode, forced_standard, forced_polar, official_conversion)
    raw = out / "MONTERO-BEAR-open-reference-bounded-5s-raw-f32.wav"
    if not args.force and marker.exists() and raw.exists():
        actual = json.loads(marker.read_text(encoding="utf-8"))
        if all(actual.get(key) == value for key, value in expected.items()):
            return raw
    out.mkdir(parents=True, exist_ok=True)
    log = out / "render.log"
    command = [args.python_exe, str(Path(__file__).with_name("render_bounded_joc_bundle.py")), str(bundle),
               "--bear-python", args.bear_python, "--data", str(data), "--output-dir", str(out),
               "--source-input", str(bundle / "metadata.jsonl")]
    for value in args.python_path:
        command.extend(("--python-path", value))
    for value in args.dll_dir:
        command.extend(("--dll-dir", value))
    if forced_standard is not None:
        command.extend(("--force-standard-position", *(str(value) for value in forced_standard)))
    if forced_polar is not None:
        command.extend(("--force-polar-position", *(str(value) for value in forced_polar)))
    if official_conversion:
        command.append("--official-ear-polar-conversion")
    with log.open("w", encoding="utf-8") as stream:
        completed = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=False)
    if completed.returncode != 0 or not raw.exists():
        raise RuntimeError(f"counterfactual BEAR failed for {mode}: {log}")
    marker_payload = {**expected, "renderSha256": sha256(raw), "render": str(raw.resolve())}
    marker.write_text(json.dumps(marker_payload, indent=2) + "\n", encoding="utf-8")
    return raw


def self_test() -> None:
    positions = [
        ((-1, 1, 0), (0.0, 0.0, 0.0), 45.0, 0.0),
        ((1, 1, 0), (1.0, 0.0, 0.0), -45.0, 0.0),
        ((0, 1, 0), (.5, 0.0, 0.0), 0.0, 0.0),
        ((-1, 0, 0), (0.0, .5, 0.0), 90.0, 0.0),
        ((1, 0, 0), (1.0, .5, 0.0), -90.0, 0.0),
        ((-1, -1, 0), (0.0, 1.0, 0.0), 135.0, 0.0),
        ((1, -1, 0), (1.0, 1.0, 0.0), -135.0, 0.0),
        ((0, 0, 1), (.5, .5, 1.0), 0.0, 90.0),
    ]
    for index, ((x, y, z), expected, azimuth, elevation) in enumerate(positions):
        actual = standard_position({"x": x, "y": y, "z": z})
        assert actual == expected
        polar = bear_polar(actual)
        assert abs(polar[0] - azimuth) < 1e-9 and abs(polar[1] - elevation) < 1e-9 and polar[2] > 0.0
        official = EXPECTED_OFFICIAL_POLAR[index]
        assert official[2] > 0.0
        assert abs(official[0] - (30.0 if index == 0 else -30.0 if index == 1 else
                                 0.0 if index in (2, 7) else
                                 70.0 if index == 3 else -70.0 if index == 4 else
                                 110.0 if index == 5 else -110.0)) < 1e-9
        assert abs(official[1] - (90.0 if index == 7 else 0.0)) < 1e-9
        if index in (3, 4, 5, 6):
            assert abs(polar[0] - official[0]) >= 20.0
    with tempfile.TemporaryDirectory() as directory:
        bundle = Path(directory)
        (bundle / "metadata.jsonl").write_text("{}\n", encoding="utf-8")
        marker = expected_marker(bundle, Path(__file__), "C", (.5, 0.0, 0.0), None)
        assert marker["mode"] == "C" and marker["forcedStandardPosition"] == [.5, 0.0, 0.0]
    print("oamdCounterfactualAuditSelfTest=PASS cases=8 provenance=1 roundTrip=1")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--summary", type=Path, default=Path("tmp/oracle/single-object-oracle-bounded-5s-r4/single-object-oracle-summary.json"))
    parser.add_argument("--output-root", type=Path, default=Path("tmp/oracle/oamd-counterfactual-r4"))
    parser.add_argument("--bear-python")
    parser.add_argument("--python-exe", default=sys.executable)
    parser.add_argument("--python-path", action="append", default=[])
    parser.add_argument("--dll-dir", action="append", default=[])
    parser.add_argument("--data", type=Path)
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if not args.bear_python or args.data is None:
        parser.error("--bear-python and --data are required unless --self-test is used")
    summary = json.loads(args.summary.read_text(encoding="utf-8"))
    manifest = json.loads(Path("tmp/oracle/single-object-cases-bounded-r3-manifest.json").read_text(encoding="utf-8"))
    official_source = Path("tmp/oracle/powder-diagnostic-ear-compatible-preserve.wav")
    official_positions = official_bear_positions(args, official_source)
    args.output_root.mkdir(parents=True, exist_ok=True)
    case_reports = []
    for case, native, source_render, joc_render in zip(summary["caseResults"], summary["native"], summary["sourceBear"], summary["jocBear"]):
        name = case["caseName"]
        bundle = Path(native["bundle"])
        manifest_case = manifest["cases"][case["selectedObjectTrackOneBased"] - 11]
        input_path = Path(manifest_case["output"])
        source_object = read_s24_object(input_path, case["selectedObjectTrackOneBased"])
        source = read_f32_stereo(Path(source_render["render"]))
        modes = {"A": Path(joc_render["render"])}
        forced_b = standard_position(case["sourceAdmCartesianPosition"])
        forced_d = tuple(official_positions[case["selectedObjectTrackOneBased"] - 11][key]
                         for key in ("azimuth", "elevation", "distance"))
        for mode, forced_standard, forced_polar, official_conversion in (("B", forced_b, None, False),
                                                                          ("C", (.5, 0.0, 0.0), None, False),
                                                                          ("D", None, forced_d, False),
                                                                          ("E", None, None, True)):
            modes[mode] = render_counterfactual(args, bundle, args.data, args.output_root / name / mode,
                                                 mode, forced_standard, forced_polar, official_conversion)
        trace = read_trace(bundle, args.output_root / name / "au-slot-trace.json")
        comparisons = {mode: compare(source_object, source, read_f32_stereo(path), mode) for mode, path in modes.items()}
        case_reports.append({"case": name, "sourceObject": case["sourceAdmObjectName"],
                             "sourcePosition": case["sourceAdmCartesianPosition"],
                             "forcedStandardPosition": forced_b,
                             "projectPolarFromStandardPosition": bear_polar(forced_b),
                             "officialEarBearPolar": official_positions[case["selectedObjectTrackOneBased"] - 11],
                             "officialAdapterSource": "pinned EAR convert_objects",
                             "trace": str((args.output_root / name / "au-slot-trace.json").resolve()),
                             "comparisons": comparisons})
    report = {"result": "PASS", "cases": case_reports,
              "interpretation": "Counterfactuals are diagnostic only; A is existing OAMD, B forces source ADM standard XYZ through project polar(), C forces front midpoint, D forces official source polar position, E preserves OAMD coordinates but uses pinned EAR point_cart_to_polar.",
              "normalizationApplied": False}
    (args.output_root / "oamd-counterfactual-summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"oamdCounterfactualAudit=PASS cases={len(case_reports)} output={args.output_root.resolve()}")


if __name__ == "__main__":
    main()
