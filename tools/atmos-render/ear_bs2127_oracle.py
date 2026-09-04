#!/usr/bin/env python3
"""Compare the pinned EAR BS.2127 point-source oracle with the local panner."""
import argparse
import json
import math
import os
import re
import subprocess
import sys


VECTOR_RE = re.compile(r"^VECTOR (\w+) (\w+) power=([^ ]+)(.*)$")

# Source: Bs2051SystemHLayout::systemH() in tools/atmos-render/bs2051-layout.cpp.
PROJECT_SPEAKER_ORDER = [
    "M+060", "M-060", "M+000", "M+135", "M-135", "M+030", "M-030",
    "M+180", "M+090", "M-090", "U+045", "U-045", "U+000", "T+000",
    "U+135", "U-135", "U+090", "U-090", "U+180", "B+000", "B+045",
    "B-045",
]


def project_vectors(executable):
    text = subprocess.check_output([executable, "--vectors"], text=True)
    result = {}
    for line in text.splitlines():
        match = VECTOR_RE.match(line.strip())
        if not match:
            continue
        if match.group(2) != "Selected":
            raise RuntimeError("local panner returned non-success status for " + match.group(1))
        result[match.group(1)] = {
            "power": float(match.group(3)),
            "gains": [float(value) for value in match.group(4).split()],
        }
    if len(result) != 7 or any(len(item["gains"]) != 22 for item in result.values()):
        raise RuntimeError("local panner did not emit seven 22-channel vectors")
    return result


def ear_vectors(source_root):
    sys.path.insert(0, source_root)
    from ear.core import bs2051, point_source  # pylint: disable=import-outside-toplevel

    layout = bs2051.get_layout("9+10+3").without_lfe
    panner = point_source.configure(layout)
    # The local contract is [front, left, up]. EAR/ADM cartesian is
    # [right, front, up]: cart(0, 0, 1) == [0, 1, 0]. Thus [x, y, z]
    # becomes [-y, x, z] before calling EAR.
    cases = {
        "front": (1.0, 0.0, 0.0),
        "left": (0.0, 1.0, 0.0),
        "right": (0.0, -1.0, 0.0),
        "rear": (-1.0, 0.0, 0.0),
        "upper": (0.0, 0.0, 1.0),
        "lower": (0.0, 0.0, -1.0),
        "interior": (0.7071067811865475, 0.5, 0.5),
    }
    result = {}
    for name, (x, y, z) in cases.items():
        gains = panner.handle([-y, x, z])
        if gains is None:
            raise RuntimeError("EAR returned no gains for " + name)
        values = [float(value) for value in gains]
        result[name] = {"power": sum(value * value for value in values), "gains": values}
    speaker_order = list(layout.channel_names)
    if speaker_order != PROJECT_SPEAKER_ORDER:
        raise RuntimeError("EAR System-H channel order differs from project contract")
    return result, speaker_order


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-exe", required=True)
    parser.add_argument("--ear-source", required=True)
    parser.add_argument("--report", required=True)
    parser.add_argument("--tolerance", type=float, default=1.0e-6)
    args = parser.parse_args()

    project = project_vectors(args.project_exe)
    ear, speaker_order = ear_vectors(os.path.abspath(args.ear_source))
    rows = []
    mismatch_count = 0
    max_error = 0.0
    finite_check = True
    for name in project:
        finite_check = finite_check and math.isfinite(project[name]["power"])
        finite_check = finite_check and all(math.isfinite(value) for value in project[name]["gains"])
        finite_check = finite_check and math.isfinite(ear[name]["power"])
        finite_check = finite_check and all(math.isfinite(value) for value in ear[name]["gains"])
        errors = [abs(a - b) for a, b in zip(project[name]["gains"], ear[name]["gains"])]
        current_max = max(errors)
        max_error = max(max_error, current_max)
        power_error = abs(project[name]["power"] - ear[name]["power"])
        mismatch = (not finite_check) or current_max > args.tolerance or power_error > args.tolerance
        mismatch_count += int(mismatch)
        rows.append({"name": name, "maxGainError": current_max, "powerError": power_error,
                     "mismatch": mismatch, "project": project[name], "ear": ear[name]})
    report = {
        "result": "PASS" if mismatch_count == 0 else "FAIL",
        "oracle": "EBU EAR 2.1.0 / BS.2127 reference point_source.configure",
        "coordinateConversion": "project [front,left,up] -> EAR [right,front,up] = [-y,x,z]",
        "speakerOrder": speaker_order,
        "orderMatch": speaker_order == PROJECT_SPEAKER_ORDER,
        "finiteCheck": finite_check,
        "tolerance": args.tolerance,
        "maxGainError": max_error,
        "mismatchCount": mismatch_count,
        "cases": rows,
    }
    with open(args.report, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(report, stream, indent=2, sort_keys=True)
        stream.write("\n")
    print(json.dumps({"result": report["result"], "cases": len(rows),
                      "mismatchCount": mismatch_count, "maxGainError": max_error}))
    return 0 if mismatch_count == 0 else 2


if __name__ == "__main__":
    raise SystemExit(main())
