"""Create a compact machine-readable and Markdown comparison of Object Direct reports."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def load(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8"))
    if value.get("schema") != "audioplayer.object-direct.v1" or value.get("result") != "PASS":
        raise ValueError(f"not a PASS Object Direct report: {path}")
    return value


def compare(left_path: Path, right_path: Path, output: Path, markdown: Path) -> dict:
    left, right = load(left_path), load(right_path)
    comparison = {
        "schema": "audioplayer.object-direct-comparison.v1",
        "result": "PASS",
        "evidenceLayer": "renderer-neutral-object-pcm-sum",
        "notAClaim": ["not binaural", "not Windows endpoint output", "not subjective listening evidence"],
        "tracks": {"left": left, "right": right},
        "comparison": {
            "sameFrameCount": left["frames"] == right["frames"],
            "leftFrames": left["frames"],
            "rightFrames": right["frames"],
            "leftPeak": left["output"]["peak"],
            "rightPeak": right["output"]["peak"],
            "leftRms": left["output"]["rms"],
            "rightRms": right["output"]["rms"],
            "leftLow200Fraction": sum(item["fraction"] for item in left["output"]["bandEnergy"][:2]),
            "rightLow200Fraction": sum(item["fraction"] for item in right["output"]["bandEnergy"][:2]),
        },
        "limitations": [
            "The 8-AU reports are startup decode smoke, not representative full-track loudness.",
            "The full Riptide report is available separately; no new full MONTERO render was run tonight.",
            "LFE inclusion is an offline sum only and does not model Windows Spatial routing.",
        ],
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(comparison, indent=2), encoding="utf-8")
    c = comparison["comparison"]
    markdown.parent.mkdir(parents=True, exist_ok=True)
    markdown.write_text(
        "# Object Direct same-source diagnostic\n\n"
        "This is a renderer-neutral dual-mono object sum, not a binaural or "
        "Windows endpoint result. The 8-AU rows are startup smoke only.\n\n"
        f"| track | frames | peak | RMS | <=200 Hz fraction |\n"
        f"|---|---:|---:|---:|---:|\n"
        f"| left | {c['leftFrames']} | {c['leftPeak']:.9g} | {c['leftRms']:.9g} | {c['leftLow200Fraction']:.6%} |\n"
        f"| right | {c['rightFrames']} | {c['rightPeak']:.9g} | {c['rightRms']:.9g} | {c['rightLow200Fraction']:.6%} |\n\n"
        "LFE is excluded by default. Riptide's decoded LFE is zero; its "
        "LFE-on/off Object Direct WAV hashes were identical. Any perceived "
        "change therefore needs endpoint loopback or another controlled A/B; "
        "it must not be attributed to LFE from this report alone.\n",
        encoding="utf-8",
    )
    return comparison


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("left", type=Path)
    parser.add_argument("right", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--markdown", type=Path, required=True)
    args = parser.parse_args()
    result = compare(args.left, args.right, args.output, args.markdown)
    print(json.dumps({"result": result["result"], "output": str(args.output.resolve()),
                      "markdown": str(args.markdown.resolve())}))


if __name__ == "__main__":
    main()
