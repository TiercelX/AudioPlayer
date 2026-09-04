"""Compare bounded normal BEAR renders against their source-object references."""

from __future__ import annotations

import argparse
import copy
import importlib.util
import json
import math
from pathlib import Path


def load_audit():
    path = Path(__file__).with_name("run_oamd_counterfactual_audit.py")
    spec = importlib.util.spec_from_file_location("oamd_audit", path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def assert_numeric(value) -> None:
    if isinstance(value, float):
        if not math.isfinite(value):
            raise ValueError("comparison report contains a nonfinite number")
    elif isinstance(value, list):
        for item in value:
            assert_numeric(item)
    elif isinstance(value, dict):
        for item in value.values():
            assert_numeric(item)
    elif isinstance(value, (str, int, bool)) or value is None:
        return
    else:
        raise TypeError(f"comparison report contains unsupported value {type(value)!r}")


def validate_report(report: dict, expected_cases: int = 8) -> None:
    if report.get("schema") != 1 or report.get("result") != "PASS":
        raise ValueError("fixed BEAR report schema/result mismatch")
    cases = report.get("cases")
    if not isinstance(cases, list) or len(cases) != expected_cases:
        raise ValueError("fixed BEAR report case count mismatch")
    assert_numeric(report)
    for case in cases:
        rows = case["comparison"]["perFrequency"]
        if not rows:
            raise ValueError("per-frequency complex gain rows are missing")
        for row in rows:
            for key in ("sourceComplexGain", "candidateComplexGain"):
                gains = row.get(key)
                if (not isinstance(gains, list) or len(gains) != 2 or
                        any(not isinstance(channel, list) or len(channel) != 2 or
                            any(isinstance(value, bool) or not isinstance(value, (int, float))
                                for value in channel) for channel in gains)):
                    raise ValueError(f"{key} contains a non-numeric complex gain")


def _valid_report() -> dict:
    row = {"frequencyHz": 401.0,
           "sourceComplexGain": [[1.0, 0.0], [0.5, -0.25]],
           "candidateComplexGain": [[0.9, 0.1], [0.4, -0.2]]}
    return {"schema": 1, "result": "PASS", "cases": [
        {"comparison": {"perFrequency": [copy.deepcopy(row), copy.deepcopy(row)]}}
        for _ in range(8)
    ]}


def self_test() -> None:
    valid = _valid_report()
    validate_report(valid)

    def rejects(mutated: dict) -> None:
        try:
            validate_report(mutated)
        except (TypeError, ValueError, KeyError):
            return
        raise AssertionError("invalid report was accepted")

    string_source = copy.deepcopy(valid)
    string_source["cases"][7]["comparison"]["perFrequency"][1]["sourceComplexGain"] = "System.Object[] System.Object[]"
    rejects(string_source)
    string_candidate = copy.deepcopy(valid)
    string_candidate["cases"][2]["comparison"]["perFrequency"][0]["candidateComplexGain"][1] = "[0.4, -0.2]"
    rejects(string_candidate)
    nonfinite = copy.deepcopy(valid)
    nonfinite["cases"][4]["comparison"]["perFrequency"][0]["candidateComplexGain"][0][1] = float("nan")
    rejects(nonfinite)
    wrong_count = copy.deepcopy(valid)
    wrong_count["cases"].pop()
    rejects(wrong_count)
    wrong_schema = copy.deepcopy(valid)
    wrong_schema["schema"] = 2
    rejects(wrong_schema)
    print("fixedBearOracleSelfTest=PASS cases=8 fullComplexGainRows=16 rejections=5")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--summary", type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--render-root", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if any(value is None for value in (args.summary, args.manifest, args.render_root, args.output)):
        parser.error("--summary, --manifest, --render-root, and --output are required unless --self-test is used")
    audit = load_audit()
    summary = json.loads(args.summary.read_text(encoding="utf-8"))
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    rows = []
    for case, source_render in zip(summary["caseResults"], summary["sourceBear"]):
        name = case["caseName"]
        input_path = Path(manifest["cases"][case["selectedObjectTrackOneBased"] - 11]["output"])
        source_object = audit.read_s24_object(input_path, case["selectedObjectTrackOneBased"])
        source = audit.read_f32_stereo(Path(source_render["render"]))
        candidate_path = args.render_root / name / "MONTERO-BEAR-open-reference-bounded-5s-raw-f32.wav"
        candidate = audit.read_f32_stereo(candidate_path)
        rows.append({"case": name, "sourceObject": case["sourceAdmObjectName"],
                     "sourcePosition": case["sourceAdmCartesianPosition"],
                     "comparison": audit.compare(source_object, source, candidate, "fixed")})
    report = {"schema": 1, "result": "PASS",
              "coordinateMapping": "pinned EAR BS.2127 port",
              "normalizationApplied": False, "cases": rows}
    validate_report(report)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"fixedNormalComparison=PASS cases={len(rows)} output={args.output.resolve()}")


if __name__ == "__main__":
    main()
