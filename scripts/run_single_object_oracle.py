"""Run the bounded eight-case single-object DEE/JOC/BEAR oracle matrix.

DEE jobs are deliberately serialized. Native probes may run concurrently, but
all outputs remain case-scoped under the selected temporary output directory.
This script does not modify source media or production code.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import struct
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path

import numpy as np
from scipy.signal import correlate, correlation_lags


RATE = 48000
FRAMES = 240000
FREQUENCIES_HZ = [173, 337, 521, 911, 1471, 2203, 3301, 4799, 6311, 7907, 9433, 11789]
DLL_DIRS = [
    "tmp/reference/bear-main-6127e897/build-visr-bear-6/python/Release",
    "tmp/reference/VISR-install/lib",
    "tmp/reference/VISR-install/3rd",
    "tmp/reference/VISR-0.13.0-build-shared/lib/Release",
    "tmp/reference/bear-git-build-shared/python/Release",
]
PYTHON_PATHS = [
    "tmp/reference/ear-2.1.0/ebu_adm_renderer-2.1.0",
    "tmp/reference/bear-main-6127e897",
    "tmp/reference/VISR-install/python",
    "tmp/reference/bear-main-6127e897/build-visr-bear-6/python/Release",
    "tmp/reference/bear-git-build-shared/python/Release",
    "tools/atmos-render",
]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def make_config(case_input: Path, ec3_output: Path, config_path: Path, temp_dir: Path) -> None:
    root = ET.Element("job_config")
    input_node = ET.SubElement(ET.SubElement(root, "input"), "audio")
    mezz = ET.SubElement(input_node, "atmos_mezz", version="1")
    ET.SubElement(mezz, "file_name").text = case_input.name
    for key, value in (("timecode_frame_rate", "not_indicated"), ("offset", "auto"), ("ffoa", "auto")):
        ET.SubElement(mezz, key).text = value
    storage = ET.SubElement(mezz, "storage")
    ET.SubElement(ET.SubElement(storage, "local"), "path").text = case_input.parent.as_posix()

    audio_filter = ET.SubElement(ET.SubElement(ET.SubElement(root, "filter"), "audio"), "encode_to_atmos_ddp", version="1")
    loudness = ET.SubElement(audio_filter, "loudness")
    measure = ET.SubElement(loudness, "measure_only")
    ET.SubElement(measure, "metering_mode").text = "1770-4"
    ET.SubElement(measure, "dialogue_intelligence").text = "false"
    ET.SubElement(measure, "speech_threshold").text = "100"
    for key, value in (("data_rate", "448"), ("timecode_frame_rate", "not_indicated"),
                       ("start", "first_frame_of_action"), ("end", "end_of_file"),
                       ("time_base", "file_position"), ("prepend_silence_duration", "0.0"),
                       ("append_silence_duration", "0.0")):
        ET.SubElement(audio_filter, key).text = value
    drc = ET.SubElement(audio_filter, "drc")
    ET.SubElement(drc, "line_mode_drc_profile").text = "none"
    ET.SubElement(drc, "rf_mode_drc_profile").text = "none"
    downmix = ET.SubElement(audio_filter, "downmix")
    for key, value in (("loro_center_mix_level", "-3"), ("loro_surround_mix_level", "-3"),
                       ("ltrt_center_mix_level", "-3"), ("ltrt_surround_mix_level", "-3"),
                       ("preferred_downmix_mode", "loro")):
        ET.SubElement(downmix, key).text = value

    output = ET.SubElement(ET.SubElement(root, "output"), "ec3", version="1")
    ET.SubElement(output, "file_name").text = ec3_output.name
    out_storage = ET.SubElement(output, "storage")
    ET.SubElement(ET.SubElement(out_storage, "local"), "path").text = ec3_output.parent.as_posix()
    misc = ET.SubElement(root, "misc")
    temp = ET.SubElement(misc, "temp_dir")
    ET.SubElement(temp, "clean_temp").text = "true"
    ET.SubElement(temp, "path").text = temp_dir.as_posix()
    config_path.parent.mkdir(parents=True, exist_ok=True)
    ET.ElementTree(root).write(config_path, encoding="utf-8", xml_declaration=True)


def probe_ec3(ffprobe: Path, ec3: Path) -> dict:
    try:
        completed = subprocess.run(
            [str(ffprobe), "-v", "error", "-show_entries", "format=duration,size",
             "-of", "json", str(ec3)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False,
        )
    except OSError as exc:
        return {"status": "FAIL", "reason": f"ffprobe-launch-failed:{exc}"}
    if completed.returncode != 0:
        return {"status": "FAIL", "reason": completed.stderr.strip() or "ffprobe-failed"}
    try:
        format_info = json.loads(completed.stdout)["format"]
        return {"status": "PASS", "durationSeconds": float(format_info["duration"]),
                "sizeBytes": int(format_info["size"])}
    except (KeyError, TypeError, ValueError, json.JSONDecodeError) as exc:
        return {"status": "FAIL", "reason": f"invalid-ffprobe-json:{exc}"}


def run_dee(case: dict, root: Path, dee: Path, ffprobe: Path, repo: Path,
            force: bool = False) -> dict:
    case_name = Path(case["output"]).stem
    case_input = Path(case["output"]).resolve()
    ec3_dir = root / "ec3"
    config_dir = root / "configs"
    log_dir = root / "dee-logs"
    ec3_dir.mkdir(parents=True, exist_ok=True)
    log_dir.mkdir(parents=True, exist_ok=True)
    ec3 = ec3_dir / f"{case_name}.ec3"
    config = config_dir / f"{case_name}.xml"
    log = log_dir / f"{case_name}.log"
    console = log_dir / f"{case_name}.console.txt"
    temp_dir = root / "dee-temp" / case_name
    temp_dir.mkdir(parents=True, exist_ok=True)
    input_sha = sha256(case_input)
    provenance = ec3.with_suffix(".input.json")
    if ec3.exists() and ec3.stat().st_size > 0:
        if not force and not provenance.exists():
            return {"case": case_name, "output": str(ec3), "inputSha256": input_sha, "exitCode": None,
                    "status": "FAIL", "reason": "stale-output-without-provenance"}
        if not force:
            try:
                prior = json.loads(provenance.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                prior = {}
            if prior.get("inputSha256") != input_sha or prior.get("inputFrames") != FRAMES:
                return {"case": case_name, "output": str(ec3), "inputSha256": input_sha, "exitCode": None,
                        "status": "FAIL", "reason": "stale-output-input-mismatch"}
            duration = probe_ec3(ffprobe, ec3)
            if duration.get("status") != "PASS" or not 4.9 <= duration.get("durationSeconds", 0.0) <= 5.1:
                return {"case": case_name, "output": str(ec3), "inputSha256": input_sha, "exitCode": None,
                        "status": "FAIL", "reason": "stale-output-duration-mismatch", "probe": duration}
            return {"case": case_name, "config": str(config), "log": str(log), "console": str(console),
                    "output": str(ec3), "inputSha256": input_sha, "exitCode": 0, "status": "PASS", "reused": True,
                    "outputSha256": sha256(ec3), "outputBytes": ec3.stat().st_size,
                    "probe": duration}
    make_config(case_input, ec3, config, temp_dir)
    with console.open("w", encoding="utf-8", errors="replace") as output:
        completed = subprocess.run(
            [str(dee), "--progress", "--stdout", "--log-file", str(log), "--xml", str(config)],
            cwd=str(dee.parent), stdout=output, stderr=subprocess.STDOUT, check=False,
        )
    probe = probe_ec3(ffprobe, ec3) if ec3.exists() else {"status": "FAIL", "reason": "missing-output"}
    status = completed.returncode == 0 and ec3.exists() and probe.get("status") == "PASS" \
        and 4.9 <= probe.get("durationSeconds", 0.0) <= 5.1
    result = {
        "case": case_name, "config": str(config), "log": str(log), "console": str(console),
        "output": str(ec3), "inputSha256": input_sha, "exitCode": completed.returncode,
        "status": "PASS" if status else "FAIL",
        "outputSha256": sha256(ec3) if ec3.exists() else None,
        "outputBytes": ec3.stat().st_size if ec3.exists() else None,
        "probe": probe,
    }
    if status:
        provenance.write_text(json.dumps({"inputSha256": input_sha, "inputFrames": FRAMES,
                                          "inputBytes": case_input.stat().st_size,
                                          "outputSha256": result["outputSha256"],
                                          "probe": probe}, indent=2) + "\n", encoding="utf-8")
    return result


def run_native(dee_result: dict, root: Path, probe: Path, repo: Path) -> dict:
    case = dee_result["case"]
    bundle = root / "native-rerun" / case
    bundle.parent.mkdir(parents=True, exist_ok=True)
    log = root / "native-rerun-logs" / f"{case}.txt"
    log.parent.mkdir(parents=True, exist_ok=True)
    provenance = bundle / "input-provenance.json"
    reusable = False
    if (bundle / "bundle.complete").exists() and log.exists() and provenance.exists():
        try:
            prior = json.loads(provenance.read_text(encoding="utf-8"))
            reusable = prior.get("inputSha256") == dee_result.get("outputSha256")
        except (OSError, json.JSONDecodeError):
            reusable = False
    if reusable:
        text = log.read_text(encoding="utf-8", errors="replace")
        match_au = re.search(r"accessUnits=(\d+)", text)
        match_b2a = re.search(r"oamdB2aDispositionPass=(\d+)", text)
        return {"case": case, "command": [], "log": str(log), "bundle": str(bundle),
                "inputSha256": dee_result.get("outputSha256"), "exitCode": 0,
                "bundleComplete": True, "accessUnits": int(match_au.group(1)) if match_au else None,
                "oamdB2aPass": int(match_b2a.group(1)) if match_b2a else None,
                "bearExport": "PASS" if "bearExport=PASS" in text else "FAIL", "status": "PASS", "reused": True}
    command = [str(probe), dee_result["output"], "--max-units", "157", "--oamd", "--joc", "--pcm",
               "--joc-gate6c", "--joc-bear-export", str(bundle)]
    with log.open("w", encoding="utf-8", errors="replace") as output:
        completed = subprocess.run(command, cwd=str(repo), stdout=output, stderr=subprocess.STDOUT, check=False)
    text = log.read_text(encoding="utf-8", errors="replace")
    result = {
        "case": case, "command": command, "log": str(log), "bundle": str(bundle),
        "inputSha256": dee_result.get("outputSha256"),
        "exitCode": completed.returncode, "bundleComplete": (bundle / "bundle.complete").exists(),
        "accessUnits": int(re.search(r"accessUnits=(\d+)", text).group(1)) if re.search(r"accessUnits=(\d+)", text) else None,
        "oamdB2aPass": int(re.search(r"oamdB2aDispositionPass=(\d+)", text).group(1)) if re.search(r"oamdB2aDispositionPass=(\d+)", text) else None,
        "bearExport": "PASS" if "bearExport=PASS" in text else "FAIL",
        "status": "PASS" if (bundle / "bundle.complete").exists() else "FAIL",
    }
    if result["status"] == "PASS":
        provenance.write_text(json.dumps({"inputSha256": dee_result.get("outputSha256"),
                                          "inputDurationSeconds": dee_result.get("probe", {}).get("durationSeconds"),
                                          "accessUnits": result["accessUnits"]}, indent=2) + "\n", encoding="utf-8")
    return result


def run_slot_analysis(native: dict, case: dict, root: Path, python: Path, repo: Path) -> dict:
    report = root / "slot-reports-rerun" / f"{native['case']}.json"
    log = root / "slot-reports-rerun" / f"{native['case']}.log"
    sequence_path = root / "slot-reports-rerun" / f"{native['case']}-oamd-position-sequence.json"
    frequency_path = root / "slot-reports-rerun" / f"{native['case']}-single-object-frequency-v2.json"
    marker = report.with_suffix(".input.json")
    reusable = False
    if report.exists() and sequence_path.exists() and frequency_path.exists() and marker.exists():
        try:
            reusable = json.loads(marker.read_text(encoding="utf-8")).get("inputSha256") == native.get("inputSha256")
        except (OSError, json.JSONDecodeError):
            reusable = False
    if reusable:
        native["slotAnalysis"] = str(report)
        native["oamdPositionSequence"] = str(sequence_path)
        native["slotFrequencyMatrix"] = str(frequency_path)
        native["slotAnalysisStatus"] = "PASS"
        return native
    report.parent.mkdir(parents=True, exist_ok=True)
    command = [str(python), str(repo / "scripts/analyze_joc_object_bundle.py"),
               str(Path(case["output"]).resolve()), native["bundle"], str(report),
               "--seconds", "5", "--object-start", "10", "--frequencies",
               *[str(value) for value in FREQUENCIES_HZ[:8]]]
    with log.open("w", encoding="utf-8", errors="replace") as stream:
        completed = subprocess.run(command, cwd=str(repo), stdout=stream, stderr=subprocess.STDOUT, check=False)
    sequence = {str(index): [] for index in range(1, 16)}
    metadata = Path(native["bundle"]) / "metadata.jsonl"
    if metadata.exists():
        with metadata.open(encoding="utf-8") as stream:
            for line in stream:
                for update in json.loads(line).get("updates", []):
                    slot = str(int(update.get("objectIndex", 0)))
                    if slot in sequence:
                        sequence[slot].append({key: update.get(key) for key in
                                               ("sourcePosition", "blockIndex", "active", "gainDb", "x", "y", "z",
                                                "standardX", "standardY", "standardZ", "rampDuration")})
    sequence_path.write_text(json.dumps({"case": native["case"], "slots": sequence}, indent=2) + "\n", encoding="utf-8")
    if completed.returncode == 0 and report.exists():
        import importlib.util
        analyzer_path = repo / "scripts/analyze_joc_object_bundle.py"
        analyzer_spec = importlib.util.spec_from_file_location("single_object_slot_analyzer", analyzer_path)
        if analyzer_spec is None or analyzer_spec.loader is None:
            raise RuntimeError(f"could not load slot analyzer: {analyzer_path}")
        analyzer = importlib.util.module_from_spec(analyzer_spec)
        analyzer_spec.loader.exec_module(analyzer)
        read_bundle, read_source = analyzer.read_bundle, analyzer.read_source
        source, rate = read_source(Path(case["output"]).resolve(), 5.0, 10, 8)
        decoded, _batches = read_bundle(Path(native["bundle"]), 15)
        selected = source[:, 10 + int(case["selectedObjectIndexZeroBased"])]
        lo, hi = int(rate * 0.5), min(source.shape[0], int(rate * 4.5), decoded.shape[1])
        basis_rows = []
        for slot_index, output in enumerate(decoded[:, lo:hi], 1):
            frequencies = []
            for frequency in FREQUENCIES_HZ:
                basis = np.exp(-2j * np.pi * frequency * np.arange(hi - lo) / rate)
                source_coeff = 2.0 * np.dot(selected[lo:hi], basis) / max(hi - lo, 1)
                output_coeff = 2.0 * np.dot(output, basis) / max(hi - lo, 1)
                frequencies.append({"frequencyHz": frequency, "sourceMagnitude": float(abs(source_coeff)),
                                    "outputMagnitude": float(abs(output_coeff)),
                                    "amplitudeRatio": float(abs(output_coeff) / max(abs(source_coeff), 1e-30)),
                                    "phaseDifferenceRad": float(np.angle(output_coeff / source_coeff)) if abs(source_coeff) > 1e-12 else None})
            basis_rows.append({"decodedObjectIndex": slot_index, "frequencies": frequencies})
        frequency_path.write_text(json.dumps({"case": native["case"], "sourceTrack": case["selectedObjectTrackOneBased"],
                                              "sampleRate": rate, "window": [lo, hi], "matrix": basis_rows}, indent=2) + "\n", encoding="utf-8")
    marker.write_text(json.dumps({"inputSha256": native.get("inputSha256"),
                                  "sourceSha256": sha256(Path(case["output"]).resolve())}, indent=2) + "\n", encoding="utf-8")
    native["slotAnalysis"] = str(report)
    native["oamdPositionSequence"] = str(sequence_path)
    native["slotFrequencyMatrix"] = str(frequency_path)
    native["slotAnalysisStatus"] = "PASS" if completed.returncode == 0 and report.exists() else "FAIL"
    return native


def python_env(repo: Path) -> dict:
    env = os.environ.copy()
    env["PYTHONPATH"] = ";".join(str((repo / path).resolve()) for path in PYTHON_PATHS)
    return env


def make_ear_compatible_case(case: dict, root: Path, python: Path, repo: Path) -> dict:
    name = Path(case["output"]).stem
    source = Path(case["output"]).resolve()
    source_axml = (repo / "tmp/oracle/powder-diagnostic-axml-ear-compatible.xml").resolve()
    axml = root / "ear-compatible" / name / "axml.xml"
    output = root / "ear-compatible" / name / f"{name}.wav"
    report = root / "ear-compatible" / name / "chunk-report.json"
    log = root / "ear-compatible-logs" / f"{name}.txt"
    output.parent.mkdir(parents=True, exist_ok=True)
    log.parent.mkdir(parents=True, exist_ok=True)
    text = source_axml.read_text(encoding="utf-8")
    text = re.sub(r'duration="[^"]+"', 'duration="00:00:05.00000"', text)
    text = re.sub(r'end="[^"]+"', 'end="01:00:05.00000"', text, count=1)
    axml.write_text(text, encoding="utf-8")
    command = [str(python), str(repo / "tmp/oracle/preserve_axml_chunks.py"), str(source), str(axml), str(output), "--report", str(report)]
    with log.open("w", encoding="utf-8", errors="replace") as stream:
        completed = subprocess.run(command, cwd=str(repo), stdout=stream, stderr=subprocess.STDOUT, check=False)
    return {"case": name, "command": command, "log": str(log), "source": str(output), "report": str(report),
            "exitCode": completed.returncode, "status": "PASS" if completed.returncode == 0 and output.exists() else "FAIL"}


def run_source_render(case: dict, source_path: str, root: Path, python: Path, repo: Path) -> dict:
    name = Path(case["output"]).stem
    source = Path(source_path).resolve()
    output = root / "source-bear" / name / "source-object-only-raw-f32.wav"
    report = root / "source-bear" / name / "source-object-only.json"
    log = root / "source-bear-logs" / f"{name}.txt"
    output.parent.mkdir(parents=True, exist_ok=True)
    log.parent.mkdir(parents=True, exist_ok=True)
    marker = output.with_suffix(".input.json")
    reusable = False
    if output.exists() and report.exists() and marker.exists():
        try:
            reusable = json.loads(marker.read_text(encoding="utf-8")).get("inputSha256") == sha256(source)
        except (OSError, json.JSONDecodeError):
            reusable = False
    if reusable:
        return {"case": name, "command": [], "log": str(log), "render": str(output), "report": str(report),
                "exitCode": 0, "status": "PASS", "reused": True}
    dlls = [str((repo / path).resolve()) for path in DLL_DIRS]
    code = "import os,runpy; [os.add_dll_directory(d) for d in " + repr(dlls) + " if os.path.isdir(d)]; runpy.run_path('tmp/oracle/render_bear_object_only.py',run_name='__main__')"
    command = [str(python), "-c", code, "--source", str(source), "--output", str(output), "--report", str(report)]
    with log.open("w", encoding="utf-8", errors="replace") as stream:
        completed = subprocess.run(command, cwd=str(repo), env=python_env(repo), stdout=stream, stderr=subprocess.STDOUT, check=False)
    result = {"case": name, "command": command, "log": str(log), "render": str(output), "report": str(report),
              "exitCode": completed.returncode, "status": "PASS" if completed.returncode == 0 and output.exists() else "FAIL"}
    if result["status"] == "PASS":
        marker.write_text(json.dumps({"inputSha256": sha256(source), "frames": FRAMES}, indent=2) + "\n", encoding="utf-8")
    return result


def run_bundle_render(native: dict, dee_result: dict, root: Path, python: Path, repo: Path, data: Path) -> dict:
    name = native["case"]
    output_dir = root / "joc-bear" / name
    log = root / "joc-bear-logs" / f"{name}.txt"
    output_dir.mkdir(parents=True, exist_ok=True)
    log.parent.mkdir(parents=True, exist_ok=True)
    render = output_dir / "MONTERO-BEAR-open-reference-bounded-5s-raw-f32.wav"
    marker = output_dir / "input-provenance.json"
    reusable = False
    if render.exists() and (output_dir / "provenance.json").exists() and marker.exists():
        try:
            reusable = json.loads(marker.read_text(encoding="utf-8")).get("inputSha256") == dee_result.get("outputSha256")
        except (OSError, json.JSONDecodeError):
            reusable = False
    if reusable:
        return {"case": name, "command": [], "log": str(log), "render": str(render),
                "exitCode": 0, "status": "PASS", "reused": True}
    command = [str(python), str(repo / "scripts/render_bounded_joc_bundle.py"), native["bundle"],
               "--bear-python", str((repo / "tmp/reference/bear-main-6127e897").resolve()), "--data", str(data.resolve()),
               "--output-dir", str(output_dir), "--source-input", dee_result["output"]]
    for path in PYTHON_PATHS:
        command.extend(["--python-path", str((repo / path).resolve())])
    for path in DLL_DIRS:
        command.extend(["--dll-dir", str((repo / path).resolve())])
    with log.open("w", encoding="utf-8", errors="replace") as stream:
        completed = subprocess.run(command, cwd=str(repo), env=python_env(repo), stdout=stream, stderr=subprocess.STDOUT, check=False)
    result = {"case": name, "command": command, "log": str(log), "render": str(render),
              "exitCode": completed.returncode, "status": "PASS" if completed.returncode == 0 and render.exists() else "FAIL"}
    if result["status"] == "PASS":
        marker.write_text(json.dumps({"inputSha256": dee_result.get("outputSha256"),
                                      "durationSeconds": dee_result.get("probe", {}).get("durationSeconds"),
                                      "frames": FRAMES}, indent=2) + "\n", encoding="utf-8")
    return result


def riff_chunks(path: Path):
    with path.open("rb") as stream:
        header = stream.read(12)
        if len(header) != 12 or header[:4] != b"RIFF" or header[8:] != b"WAVE":
            raise ValueError("expected RIFF/WAVE")
        while True:
            header = stream.read(8)
            if not header:
                return
            name, size = header[:4], struct.unpack("<I", header[4:])[0]
            offset = stream.tell()
            yield name, size, offset
            stream.seek(size + (size & 1), 1)


def validate_case_wav(path: Path) -> dict:
    table = {name: (size, offset) for name, size, offset in riff_chunks(path)}
    fmt_size, fmt_offset = table[b"fmt "]
    with path.open("rb") as stream:
        stream.seek(fmt_offset)
        audio_format, channels, rate, _br, block, bits = struct.unpack(
            "<HHIIHH", stream.read(fmt_size))
    data_size, _data_offset = table[b"data"]
    expected_bytes = FRAMES * 18 * 3
    if (audio_format, channels, rate, block, bits) != (1, 18, RATE, 54, 24):
        raise ValueError("case must be 18-channel PCM S24LE at 48 kHz")
    if data_size != expected_bytes:
        raise ValueError(f"case data must be exactly {expected_bytes} bytes, got {data_size}")
    return {"frames": data_size // block, "durationSeconds": data_size / block / rate,
            "dataBytes": data_size, "sha256": sha256(path), "fileBytes": path.stat().st_size}


def read_s24(path: Path, frames: int) -> np.ndarray:
    table = {name: (size, offset) for name, size, offset in riff_chunks(path)}
    fmt_size, fmt_offset = table[b"fmt "]
    with path.open("rb") as stream:
        stream.seek(fmt_offset)
        audio_format, channels, rate, _br, block, bits = struct.unpack("<HHIIHH", stream.read(fmt_size))
        if (audio_format, channels, rate, block, bits) != (1, 18, RATE, 54, 24):
            raise ValueError("unexpected case format")
        data_size, data_offset = table[b"data"]
        count = min(frames, data_size // block)
        stream.seek(data_offset)
        raw = np.frombuffer(stream.read(count * block), dtype=np.uint8).reshape(count, channels, 3)
    value = raw[:, :, 0].astype(np.int32) | (raw[:, :, 1].astype(np.int32) << 8) | (raw[:, :, 2].astype(np.int32) << 16)
    value[value >= (1 << 23)] -= 1 << 24
    return value.astype(np.float64) / float(1 << 23)


def read_f32_stereo(path: Path, frames: int) -> np.ndarray:
    table = {name: (size, offset) for name, size, offset in riff_chunks(path)}
    fmt_size, fmt_offset = table[b"fmt "]
    with path.open("rb") as stream:
        stream.seek(fmt_offset)
        audio_format, channels, rate, _br, block, bits = struct.unpack("<HHIIHH", stream.read(fmt_size))
        if (audio_format, channels, rate, block, bits) != (3, 2, RATE, 8, 32):
            raise ValueError("unexpected BEAR format")
        data_size, data_offset = table[b"data"]
        count = min(frames, data_size // block)
        stream.seek(data_offset)
        return np.frombuffer(stream.read(count * block), dtype="<f4").reshape(count, 2).astype(np.float64)


def response_metrics(output: np.ndarray, source: np.ndarray, frequency: float) -> dict:
    start, end = int(0.5 * RATE), int(4.5 * RATE)
    y, x = output[start:end], source[start:end]
    basis = np.exp(-2j * np.pi * frequency * np.arange(len(x)) / RATE)
    gain = np.dot(y, basis) / np.dot(x, basis)
    cross = correlate(y - np.mean(y), x - np.mean(x), mode="full", method="fft")
    lags = correlation_lags(len(y), len(x), mode="full")
    index = int(np.argmax(np.abs(cross)))
    scalar = float(np.dot(y, x) / max(np.dot(x, x), 1e-30))
    residual = y - scalar * x
    return {"complexGain": {"real": float(gain.real), "imag": float(gain.imag), "magnitude": float(abs(gain)), "phaseRad": float(np.angle(gain))},
            "waveformCorrelation": float(cross[index] / np.sqrt(np.dot(y - np.mean(y), y - np.mean(y)) * np.dot(x - np.mean(x), x - np.mean(x)))),
            "lagSamples": int(lags[index]), "bestScalarGain": scalar,
            "residualRms": float(np.sqrt(np.mean(residual * residual))), "residualEnergy": float(np.sum(residual * residual))}


def wideband_metrics(output: np.ndarray, reference: np.ndarray) -> dict:
    start, end = int(0.5 * RATE), int(4.5 * RATE)
    y, x = output[start:end], reference[start:end]
    y0, x0 = y - np.mean(y), x - np.mean(x)
    cross = correlate(y0, x0, mode="full", method="fft")
    lags = correlation_lags(len(y0), len(x0), mode="full")
    index = int(np.argmax(np.abs(cross)))
    scalar = float(np.dot(y, x) / max(np.dot(x, x), 1e-30))
    residual = y - scalar * x
    return {"waveformCorrelation": float(cross[index] / np.sqrt(np.dot(y0, y0) * np.dot(x0, x0))),
            "lagSamples": int(lags[index]), "bestScalarGain": scalar,
            "residualRms": float(np.sqrt(np.mean(residual * residual))),
            "residualEnergy": float(np.sum(residual * residual))}


def endpoint_report(case: dict, source_render: dict, joc_render: dict, root: Path) -> dict:
    source = read_s24(Path(case["output"]).resolve(), FRAMES)
    source_objects = source[:, 10:18]
    selected_source = source_objects[:, int(case["selectedObjectIndexZeroBased"])]
    source_bear = read_f32_stereo(Path(source_render["render"]), FRAMES)
    joc_bear = read_f32_stereo(Path(joc_render["render"]), FRAMES)
    rows = []
    for index, frequency in enumerate(FREQUENCIES_HZ):
        source_response = {"left": response_metrics(source_bear[:, 0], selected_source, frequency),
                           "right": response_metrics(source_bear[:, 1], selected_source, frequency)}
        joc_response = {"left": response_metrics(joc_bear[:, 0], selected_source, frequency),
                        "right": response_metrics(joc_bear[:, 1], selected_source, frequency)}
        ild_source = 20 * np.log10(max(source_response["left"]["complexGain"]["magnitude"], 1e-30) / max(source_response["right"]["complexGain"]["magnitude"], 1e-30))
        ild_joc = 20 * np.log10(max(joc_response["left"]["complexGain"]["magnitude"], 1e-30) / max(joc_response["right"]["complexGain"]["magnitude"], 1e-30))
        ipd_source = np.angle(complex(source_response["left"]["complexGain"]["real"], source_response["left"]["complexGain"]["imag"]) / complex(source_response["right"]["complexGain"]["real"], source_response["right"]["complexGain"]["imag"]))
        ipd_joc = np.angle(complex(joc_response["left"]["complexGain"]["real"], joc_response["left"]["complexGain"]["imag"]) / complex(joc_response["right"]["complexGain"]["real"], joc_response["right"]["complexGain"]["imag"]))
        rows.append({"frequencyIndex": index, "objectTrack": case["selectedObjectTrackOneBased"],
                     "frequencyHz": frequency, "sourceObjectReference": source_response,
                     "jocProgramme": joc_response, "deltaIldDb": float(ild_joc - ild_source),
                     "deltaIpdRad": float(np.angle(np.exp(1j * (ipd_joc - ipd_source))))})
    report = {"case": Path(case["output"]).stem, "result": "MEASURED", "framesCompared": FRAMES, "sampleRate": RATE,
              "rendererLatencyCompensationSamples": 167,
              "comparisonAdditionalLatencyCompensationSamples": 0, "normalizationApplied": False,
              "sourceObjectReference": {"status": "PASS", "render": source_render["render"], "sha256": sha256(Path(source_render["render"]))},
              "jocProgramme": {"status": "PASS", "render": joc_render["render"], "sha256": sha256(Path(joc_render["render"]))},
              "sourceObjectPeak": [float(np.max(np.abs(source_bear[:, c]))) for c in range(2)],
              "jocPeak": [float(np.max(np.abs(joc_bear[:, c]))) for c in range(2)],
              "peakRatio": [float(np.max(np.abs(joc_bear[:, c])) / max(np.max(np.abs(source_bear[:, c])), 1e-30)) for c in range(2)],
              "rmsRatio": [float(np.sqrt(np.mean(joc_bear[:, c] ** 2)) / max(np.sqrt(np.mean(source_bear[:, c] ** 2)), 1e-30)) for c in range(2)],
              "wideband": {"left": wideband_metrics(joc_bear[:, 0], source_bear[:, 0]), "right": wideband_metrics(joc_bear[:, 1], source_bear[:, 1])},
              "perFrequency": rows,
              "interpretation": "Endpoint difference is measured without normalization; root cause remains open between JOC PCM and OAMD/slot mapping."}
    path = root / "endpoint-reports" / f"{report['case']}.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def case_matrix(case: dict, native: dict, endpoint: dict, root: Path) -> dict:
    selected = int(case["selectedObjectIndexZeroBased"])
    slot_report = json.loads(Path(native["slotAnalysis"]).read_text(encoding="utf-8"))
    candidates = []
    for slot_index, row in enumerate(slot_report["correlationLagGainMatrix"], 1):
        metric = row[selected]
        correlation = metric.get("correlation")
        if isinstance(correlation, (int, float)) and np.isfinite(correlation):
            candidates.append({"decodedSlot": slot_index, "correlation": float(correlation),
                               "lagSamples": metric.get("lagSamples"), "gain": metric.get("gain"),
                               "outputRms": metric.get("outputRms")})
    candidates.sort(key=lambda item: abs(item["correlation"]), reverse=True)
    sequence = json.loads(Path(native["oamdPositionSequence"]).read_text(encoding="utf-8"))["slots"]
    position_summary = {}
    for slot, updates in sequence.items():
        noncenter = [item for item in updates if any(abs(float(item.get(axis) or 0.0)) > 1e-12 for axis in ("x", "y", "z"))]
        position_summary[slot] = {"updates": len(updates), "nonCenterUpdates": len(noncenter),
                                  "au0": updates[0] if updates else None, "au1": updates[1] if len(updates) > 1 else None}
    source_positions = {"L": {"x": -1.0, "y": 1.0, "z": 0.0}, "R": {"x": 1.0, "y": 1.0, "z": 0.0},
                        "C": {"x": 0.0, "y": 1.0, "z": 0.0}, "Lm": {"x": -1.0, "y": 0.0, "z": 0.0},
                        "Rm": {"x": 1.0, "y": 0.0, "z": 0.0}, "Ls": {"x": -1.0, "y": -1.0, "z": 0.0},
                        "Rs": {"x": 1.0, "y": -1.0, "z": 0.0}, "Mono": {"x": 0.0, "y": 0.0, "z": 1.0}}
    source_name = ["L", "R", "C", "Lm", "Rm", "Ls", "Rs", "Mono"][selected]
    return {"case": case["selectedObjectTrackOneBased"], "caseName": Path(case["output"]).stem,
            "selectedObjectTrackOneBased": case["selectedObjectTrackOneBased"],
            "sourceAdmObjectName": source_name, "sourceAdmCartesianPosition": source_positions[source_name],
            "stimulus": case["stimulus"], "native": {"accessUnits": native["accessUnits"], "oamdB2aPass": native["oamdB2aPass"], "bearExport": native["bearExport"]},
            "bestSlotCandidates": candidates[:5], "oamdPositionSummaryBySlot": position_summary,
            "endpoint": {"peakRatio": endpoint["peakRatio"], "rmsRatio": endpoint["rmsRatio"], "wideband": endpoint["wideband"],
                          "deltaIldDb": [item["deltaIldDb"] for item in endpoint["perFrequency"]],
                          "deltaIpdRad": [item["deltaIpdRad"] for item in endpoint["perFrequency"]]}}


def self_test() -> None:
    """Exercise the pure metric helpers without touching media or tools."""
    samples = np.arange(FRAMES, dtype=np.float64)
    signal = np.sin(2.0 * np.pi * 607.0 * samples / RATE)
    metrics = wideband_metrics(signal, signal)
    assert abs(metrics["waveformCorrelation"] - 1.0) < 1e-12
    assert metrics["lagSamples"] == 0
    assert abs(metrics["bestScalarGain"] - 1.0) < 1e-12
    assert metrics["residualRms"] < 1e-12
    assert len(FREQUENCIES_HZ) == 12
    assert FRAMES == 5 * RATE
    print("singleObjectOracleOrchestrationSelfTest=PASS cases=3")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, default=Path("tmp/oracle/single-object-cases-bounded-r3-manifest.json"))
    parser.add_argument("--output-root", type=Path, default=Path("tmp/oracle/single-object-oracle-bounded-5s-r3"))
    parser.add_argument("--dee", type=Path, default=Path("E:/Tool/dolby_encoding_engine/dee.exe"))
    parser.add_argument("--probe", type=Path, default=Path("build-mm/Debug/Eac3AccessUnitProbe.exe"))
    parser.add_argument("--ffprobe", type=Path, default=Path("build-mm/ffmpeg-audio-core/runtime-with-ffprobe-msvc/bin/ffprobe.exe"))
    parser.add_argument("--python", type=Path, default=Path("tmp/reference/ear-2.1.0/venv/Scripts/python.exe"))
    parser.add_argument("--repo", type=Path, default=Path("."))
    parser.add_argument("--case", help="run one manifest case by stem")
    parser.add_argument("--force", action="store_true", help="allow replacing existing output in the selected tmp root")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    repo = args.repo.resolve()
    root = (repo / args.output_root).resolve()
    manifest = (repo / args.manifest).resolve()
    data = repo / "tmp/reference/bear-main-6127e897/data/default_v1.1.tf"
    cases = json.loads(manifest.read_text(encoding="utf-8"))["cases"]
    if args.case:
        cases = [case for case in cases if Path(case["output"]).stem == args.case]
        if not cases:
            raise SystemExit(f"case not found in manifest: {args.case}")
    expected_case_count = len(cases)
    validate = []
    for case in cases:
        physical = validate_case_wav(Path(case["output"]).resolve())
        validate.append({"case": Path(case["output"]).stem, "bedPeak": 0.0,
                         "selectedTrack": case["selectedObjectTrackOneBased"],
                         "stimulusPeak": case["stimulus"]["peak"], "physical": physical})

    dee_results = []
    for case in cases:
        result = run_dee(case, root, (repo / args.dee).resolve(),
                         (repo / args.ffprobe).resolve(), repo, args.force)
        dee_results.append(result)
    native_results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
        futures = [executor.submit(run_native, result, root, (repo / args.probe).resolve(), repo) for result in dee_results if result["status"] == "PASS"]
        for future in futures:
            native_results.append(future.result())
    native_by_case = {item["case"]: item for item in native_results}
    cases_by_name = {Path(item["output"]).stem: item for item in cases}
    for native in native_results:
        if native["status"] == "PASS":
            run_slot_analysis(native, cases_by_name[native["case"]], root, (repo / args.python).resolve(), repo)
    source_results, compatibility_results, joc_results, endpoint_results = [], [], [], []
    for case in cases:
        name = Path(case["output"]).stem
        if name not in native_by_case or native_by_case[name]["status"] != "PASS":
            continue
        compatibility = make_ear_compatible_case(case, root, (repo / args.python).resolve(), repo)
        compatibility_results.append(compatibility)
        if compatibility["status"] != "PASS":
            continue
        source = run_source_render(case, compatibility["source"], root, (repo / args.python).resolve(), repo)
        source_results.append(source)
        if source["status"] != "PASS":
            continue
        joc = run_bundle_render(native_by_case[name], next(item for item in dee_results if item["case"] == name), root, (repo / args.python).resolve(), repo, data)
        joc_results.append(joc)
        if joc["status"] == "PASS":
            endpoint_results.append(endpoint_report(case, source, joc, root))
    endpoint_by_case = {item["case"]: item for item in endpoint_results}
    case_results = [case_matrix(cases_by_name[name], native_by_case[name], endpoint_by_case[name], root)
                    for name in sorted(endpoint_by_case) if name in native_by_case and "slotAnalysis" in native_by_case[name]]
    summary = {"result": "PASS" if len(dee_results) == expected_case_count and all(x["status"] == "PASS" for x in dee_results) and len(endpoint_results) == expected_case_count else "INCONCLUSIVE",
               "caseCount": expected_case_count, "sourceValidation": validate, "dee": dee_results, "native": native_results,
               "earCompatible": compatibility_results, "sourceBear": source_results, "jocBear": joc_results,
               "endpointReports": [str(root / "endpoint-reports" / f"{x['case']}.json") for x in endpoint_results],
               "caseResults": case_results,
               "rootCause": "INCONCLUSIVE: distinguish JOC PCM coding from OAMD/slot mapping with native slot matrix and endpoint evidence.",
               "notes": ["Original media was not modified.", "All audio/config/log/report outputs are under tmp.", "DEE was serialized; native probe workers were bounded to four."]}
    root.mkdir(parents=True, exist_ok=True)
    summary_path = root / "single-object-oracle-summary.json"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"result": summary["result"], "caseCount": expected_case_count, "deePass": sum(x["status"] == "PASS" for x in dee_results), "endpointReports": len(endpoint_results), "summary": str(summary_path)}))


if __name__ == "__main__":
    main()
