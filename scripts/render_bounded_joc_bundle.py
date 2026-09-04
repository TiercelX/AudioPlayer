"""Run the existing official BEAR bundle renderer on a bounded 5 s window."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import sys
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--bear-python", required=True)
    parser.add_argument("--python-path", action="append", default=[])
    parser.add_argument("--data", required=True)
    parser.add_argument("--dll-dir", action="append", default=[])
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--source-input", required=True)
    parser.add_argument("--force-standard-position", type=float, nargs=3,
                        metavar=("X", "Y", "Z"),
                        help="diagnostic counterfactual: force every slot to one standard position")
    parser.add_argument("--force-polar-position", type=float, nargs=3,
                        metavar=("AZ", "EL", "DIST"),
                        help="diagnostic counterfactual: force every slot to BEAR polar position")
    parser.add_argument("--official-ear-polar-conversion", action="store_true",
                        help="diagnostic counterfactual: use pinned EAR point_cart_to_polar per metadata update")
    args = parser.parse_args()
    if sum(value is not None for value in (args.force_standard_position, args.force_polar_position)) + int(args.official_ear_polar_conversion) > 1:
        parser.error("choose only one counterfactual position representation")

    renderer_path = Path(__file__).resolve().parents[1] / "tools" / "atmos-render" / "run_bear_montero_bundle.py"
    spec = importlib.util.spec_from_file_location("bounded_bundle_renderer", renderer_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load official bundle renderer: {renderer_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.WINDOWS = ((0, 240000, "bounded-5s"),)
    if args.force_standard_position is not None:
        original_apply = module.apply_bear_metadata
        forced = tuple(float(value) for value in args.force_standard_position)

        def apply_forced_position(item, state, current, duration):
            replacement = dict(state)
            replacement["standardX"], replacement["standardY"], replacement["standardZ"] = forced
            return original_apply(item, replacement, current, duration)

        module.apply_bear_metadata = apply_forced_position
    if args.force_polar_position is not None:
        forced = tuple(float(value) for value in args.force_polar_position)

        def forced_polar(_state):
            return forced

        module.polar = forced_polar
    if args.official_ear_polar_conversion:
        for python_path in args.python_path:
            resolved = str(Path(python_path).resolve())
            if resolved not in sys.path:
                sys.path.insert(0, resolved)
        from ear.core.objectbased.conversion import point_cart_to_polar

        def official_polar(state):
            x = 2.0 * float(state.get("standardX", .5)) - 1.0
            y = 1.0 - 2.0 * float(state.get("standardY", .5))
            z = float(state.get("standardZ", 0.0))
            return tuple(float(value) for value in point_cart_to_polar(x, y, z))

        module.polar = official_polar
    sys.argv = [str(renderer_path), str(args.bundle), "--bear-python", args.bear_python,
                "--data", args.data, "--output-dir", str(args.output_dir)]
    for item in args.python_path:
        sys.argv.extend(["--python-path", item])
    for item in args.dll_dir:
        sys.argv.extend(["--dll-dir", item])
    module.main()

    provenance = args.output_dir / "provenance.json"
    if provenance.exists():
        report = json.loads(provenance.read_text(encoding="utf-8"))
        source_input = Path(args.source_input).resolve()
        report["sourceInput"] = str(source_input)
        digest = hashlib.sha256()
        with source_input.open("rb") as stream:
            for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
                digest.update(block)
        report["sourceSha256"] = digest.hexdigest()
        provenance.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
