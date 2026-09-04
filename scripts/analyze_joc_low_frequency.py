#!/usr/bin/env python3
"""Audit low-QMF-band energy and matrix coherence from a JOC trace.

This is a diagnostic accounting tool, not an acoustic energy-conservation
proof: object rows can intentionally duplicate/transform a source.  The
trace's 64-band QMF is reported at its native resolution (nominally 375 Hz at
48 kHz), so this tool never labels band 0 as <=80 Hz or 80-200 Hz.
"""

from __future__ import annotations

import argparse
import json
import math
import tempfile
import unittest
from pathlib import Path
from typing import Any


LOW_BANDS = (0, 1)


def _finite(value: Any, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) \
            or not math.isfinite(float(value)):
        raise ValueError(f"{label} is not finite")
    return float(value)


def _load(path: Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    with path.open(encoding="utf-8") as stream:
        records = [json.loads(line) for line in stream if line.strip()]
    if not records or records[0].get("recordType") != "provenance":
        raise ValueError("missing provenance")
    if records[0].get("schema") != "joc-matrix-trace-v1":
        raise ValueError("unsupported trace schema")
    aus = records[1:]
    if not aus or any(item.get("recordType") != "au" for item in aus):
        raise ValueError("trace has no AU records")
    return records[0], aus


def _band_template(item: dict[str, Any]) -> dict[str, Any]:
    subband = item.get("subband")
    if subband not in LOW_BANDS:
        raise ValueError("unexpected low-band subband")
    low = _finite(item.get("nominalLowHz"), "nominalLowHz")
    high = _finite(item.get("nominalHighHz"), "nominalHighHz")
    qin = item.get("qinEnergy")
    objects = item.get("objects")
    if not isinstance(qin, list) or not isinstance(objects, list):
        raise ValueError("low-band qin/objects must be arrays")
    return {"subband": int(subband), "nominalLowHz": low,
            "nominalHighHz": high, "qin": qin, "objects": objects}


def analyze(path: Path) -> dict[str, Any]:
    provenance, aus = _load(path)
    aggregate: dict[int, dict[str, Any]] = {}
    input_identities = None
    object_count = None
    lfe_energy = 0.0
    lfe_samples = 0
    lfe_nonzero = 0
    reconstruction_max = 0.0
    reconstruction_mismatches = 0
    for au_index, au in enumerate(aus):
        bands = au.get("qmfLowSubbands")
        if not isinstance(bands, list) or len(bands) != len(LOW_BANDS):
            raise ValueError(f"AU {au_index} lacks complete low-band audit")
        identities = au.get("inputIdentities")
        if not isinstance(identities, list) or not identities:
            raise ValueError(f"AU {au_index} lacks input identities")
        if input_identities is None:
            input_identities = identities
        elif input_identities != identities:
            raise ValueError("input identities changed")
        lfe = au.get("lfePcm")
        if not isinstance(lfe, dict):
            raise ValueError(f"AU {au_index} lacks independent LFE accounting")
        lfe_energy += _finite(lfe.get("energy"), "lfe.energy")
        lfe_samples += int(lfe.get("sampleCount", 0))
        lfe_nonzero += int(lfe.get("nonzeroSamples", 0))
        check = au.get("reconstructionCheck", {})
        reconstruction_max = max(reconstruction_max,
                                 _finite(check.get("maxAbs", 0.0), "reconstruction.maxAbs"))
        reconstruction_mismatches += int(check.get("mismatchCount", 0))
        for raw_band in bands:
            band = _band_template(raw_band)
            key = band["subband"]
            if key in aggregate and len(band["qin"]) != len(input_identities):
                raise ValueError("Qin channel count changed")
            if key not in aggregate:
                aggregate[key] = {
                    "subband": key, "nominalLowHz": band["nominalLowHz"],
                    "nominalHighHz": band["nominalHighHz"],
                    "qinEnergy": [0.0] * len(band["qin"]), "objects": []}
            out = aggregate[key]
            for index, value in enumerate(band["qin"]):
                out["qinEnergy"][index] += _finite(value, "qinEnergy")
            if object_count is None:
                object_count = len(band["objects"])
                out["objects"] = [{"object": i, "presentAUs": 0,
                                    "incoherentBaselineEnergy": 0.0,
                                    "coherentExpectedEnergy": 0.0,
                                    "actualQoutEnergy": 0.0,
                                    "crossTermEnergy": 0.0,
                                    "maxRowNorm": 0.0,
                                    "maxNonzeroCoefficientsPerRow": 0,
                                    "multiInputRowTimeslots": 0,
                                    "maxAbsCoefficient": 0.0}
                                   for i in range(object_count)]
            elif not out["objects"]:
                out["objects"] = [{"object": i, "presentAUs": 0,
                                    "incoherentBaselineEnergy": 0.0,
                                    "coherentExpectedEnergy": 0.0,
                                    "actualQoutEnergy": 0.0,
                                    "crossTermEnergy": 0.0,
                                    "maxRowNorm": 0.0,
                                    "maxNonzeroCoefficientsPerRow": 0,
                                    "multiInputRowTimeslots": 0,
                                    "maxAbsCoefficient": 0.0}
                                   for i in range(object_count)]
            if len(band["objects"]) != object_count:
                raise ValueError("object count changed")
            for obj_index, raw_object in enumerate(band["objects"]):
                obj = out["objects"][obj_index]
                if raw_object.get("present"):
                    obj["presentAUs"] += 1
                for field in ("incoherentBaselineEnergy", "coherentExpectedEnergy",
                              "actualQoutEnergy", "crossTermEnergy"):
                    obj[field] += _finite(raw_object.get(field), field)
                obj["maxRowNorm"] = max(obj["maxRowNorm"],
                                         _finite(raw_object.get("maxRowNorm"), "maxRowNorm"))
                obj["maxNonzeroCoefficientsPerRow"] = max(
                    obj["maxNonzeroCoefficientsPerRow"],
                    int(raw_object.get("maxNonzeroCoefficientsPerRow", 0)))
                obj["multiInputRowTimeslots"] += int(
                    raw_object.get("multiInputRowTimeslots", 0))
                obj["maxAbsCoefficient"] = max(obj["maxAbsCoefficient"],
                                                _finite(raw_object.get("maxAbsCoefficient"), "maxAbsCoefficient"))
    band_results = []
    total_actual = 0.0
    total_incoherent = 0.0
    total_coherent = 0.0
    max_row_norm = 0.0
    for subband in LOW_BANDS:
        item = aggregate[subband]
        qin_total = sum(item["qinEnergy"])
        actual = sum(obj["actualQoutEnergy"] for obj in item["objects"])
        incoherent = sum(obj["incoherentBaselineEnergy"] for obj in item["objects"])
        coherent = sum(obj["coherentExpectedEnergy"] for obj in item["objects"])
        cross = sum(obj["crossTermEnergy"] for obj in item["objects"])
        total_actual += actual
        total_incoherent += incoherent
        total_coherent += coherent
        max_row_norm = max(max_row_norm, *(obj["maxRowNorm"] for obj in item["objects"]))
        band_results.append({**item, "qinTotalEnergy": qin_total,
                             "objectActualTotalEnergy": actual,
                             "objectIncoherentBaselineEnergy": incoherent,
                             "objectCoherentExpectedEnergy": coherent,
                             "crossTermEnergy": cross,
                             "actualToQinRatio": actual / qin_total if qin_total else None,
                             "coherentToIncoherentRatio": coherent / incoherent if incoherent else None})
    if lfe_energy == 0.0:
        lfe_result = "ZERO_LFE_INPUT_AND_NO_LFE_TERM_IN_FORMULA"
    else:
        lfe_result = "NONZERO_LFE_EXCLUDED_FROM_TRACED_QMF_RECONSTRUCTION"
    qin_low_total = sum(item["qinTotalEnergy"] for item in band_results)
    max_nonzero = max((obj["maxNonzeroCoefficientsPerRow"]
                       for item in band_results for obj in item["objects"]), default=0)
    multi_input = sum(obj["multiInputRowTimeslots"]
                      for item in band_results for obj in item["objects"])
    return {
        "schema": "audioplayer.joc-low-frequency-energy-audit.v1",
        "result": "PASS_ACCOUNTING" if reconstruction_mismatches == 0 else "INCONCLUSIVE_RECONSTRUCTION_MISMATCH",
        "evidenceLayer": "internal-qmf-matrix-accounting",
        "trace": str(path.resolve()), "sourcePath": provenance.get("sourcePath"),
        "accessUnits": len(aus), "inputIdentities": input_identities,
        "numObjects": object_count, "sampleRateHz": provenance.get("qmfSampleRateHz", 48000),
        "qmfBandDefinition": {
            "subbands": 64, "nominalSpacingHz": 375,
            "auditedSubbands": [0, 1], "nominalRangesHz": [[0, 375], [375, 750]],
            "cannotResolve80HzOr200HzBoundaries": True,
            "timeDomain80HzBands": "NOT_MEASURED_BY_THIS_TRACE_ANALYZER",
        },
        "bands": band_results,
        "totals": {"qinLowBandEnergy": qin_low_total,
                   "objectActualEnergy": total_actual,
                   "objectIncoherentBaselineEnergy": total_incoherent,
                   "objectCoherentExpectedEnergy": total_coherent,
                   "crossTermEnergy": total_coherent - total_incoherent,
                   "actualToQinRatio": total_actual / qin_low_total if qin_low_total else None,
                   "maxNonzeroCoefficientsPerRow": max_nonzero,
                   "multiInputRowTimeslots": multi_input,
                   "actualToIncoherentRatio": total_actual / total_incoherent if total_incoherent else None,
                   "maxMatrixRowNorm": max_row_norm},
        "lfe": {"result": lfe_result, "sampleCount": lfe_samples,
                "energy": lfe_energy, "nonzeroSamples": lfe_nonzero,
                "note": "Qout is reconstructed from non-LFE Qin only. A zero residual with nonzero independent LFE is internal evidence that the traced reconstruction contains no LFE term; this is not an external Dolby oracle."},
        "reconstruction": {"maxAbs": reconstruction_max, "mismatchCount": reconstruction_mismatches},
        "conclusion": {
            "lfeLeakage": "NO_LFE_TERM_IN_TRACED_QMF_RECONSTRUCTION when reconstruction maxAbs=0; internal traced-AU result, not an external Dolby oracle",
            "mainChannelLowFrequencyContent": "SEE_QIN_LOW_QMF_BANDS",
            "matrixAmplification": "REPORTS_INCOHERENT_BASELINE_AND_COHERENT_ACTUAL; object sum is not acoustic conservation",
            "qmfError": "PASS" if reconstruction_mismatches == 0 else "INCONCLUSIVE",
        },
    }


def markdown(report: dict[str, Any]) -> str:
    totals = report["totals"]
    lines = ["# JOC low-frequency energy audit", "", f"Result: `{report['result']}`",
             f"Trace: `{report['trace']}`", f"Access units: `{report['accessUnits']}`", "",
             "QMF bands are nominal 0-375 Hz (subband 0) and 375-750 Hz (subband 1) at 48 kHz. "
             "This cannot resolve <=80 Hz versus 80-200 Hz; no such split is claimed.", "",
             "| Band | Qin energy | object actual | incoherent baseline | coherent expected | cross term | actual/Qin |",
             "|---:|---:|---:|---:|---:|---:|---:|"]
    for band in report["bands"]:
        lines.append("| {subband} ({nominalLowHz:g}-{nominalHighHz:g} Hz) | {qinTotalEnergy:.9g} | {objectActualTotalEnergy:.9g} | {objectIncoherentBaselineEnergy:.9g} | {objectCoherentExpectedEnergy:.9g} | {crossTermEnergy:.9g} | {actualToQinRatio} |".format(**band))
    lines += ["", f"LFE: `{report['lfe']['result']}`, energy `{report['lfe']['energy']:.9g}`, nonzero samples `{report['lfe']['nonzeroSamples']}`.",
              f"Totals: actual/Qin ratio `{totals['actualToQinRatio']}`; actual/incoherent ratio `{totals['actualToIncoherentRatio']}`; maximum matrix row norm `{totals['maxMatrixRowNorm']:.9g}`; maximum nonzero coefficients per row `{totals['maxNonzeroCoefficientsPerRow']}`; multi-input row-timeslots `{totals['multiInputRowTimeslots']}`.",
              "", "A zero reconstruction residual means Qout is reconstructed by the traced non-LFE Qin matrix formula for these AUs. It does not prove external Dolby equivalence or exclude upstream folding before the trace.",
              "This is internal QMF accounting, not an acoustic energy-conservation, endpoint, or listening-quality proof."]
    return "\n".join(lines) + "\n"


class LowFrequencyAuditTests(unittest.TestCase):
    def _trace(self, qin: list[list[float]], objects: list[dict[str, Any]], lfe: float = 0.0) -> Path:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / "trace.jsonl"
        provenance = {"recordType": "provenance", "schema": "joc-matrix-trace-v1",
                      "sourcePath": "fixture.ec3", "qmfSampleRateHz": 48000}
        bands = []
        for subband in LOW_BANDS:
            bands.append({"subband": subband, "nominalLowHz": subband * 375,
                          "nominalHighHz": (subband + 1) * 375,
                          "qinEnergy": qin[subband], "objects": objects[subband]})
        au = {"recordType": "au", "inputIdentities":["FL", "FR"],
              "qmfLowSubbands": bands, "lfePcm":{"sampleCount":1536,"energy":lfe,"nonzeroSamples":0},
              "reconstructionCheck":{"maxAbs":0.0,"mismatchCount":0}}
        path.write_text(json.dumps(provenance) + "\n" + json.dumps(au) + "\n", encoding="utf-8")
        return path

    @staticmethod
    def _obj(actual: float, baseline: float, coherent: float, cross: float,
             row: float = 1.0) -> dict[str, Any]:
        return {"present": True, "incoherentBaselineEnergy": baseline,
                "coherentExpectedEnergy": coherent, "actualQoutEnergy": actual,
                "crossTermEnergy": cross, "maxRowNorm": row, "maxAbsCoefficient": row}

    def test_zero_lfe_identity(self):
        objects = [[self._obj(1.0, 1.0, 1.0, 0.0)], [self._obj(0.0, 0.0, 0.0, 0.0)]]
        report = analyze(self._trace([[1.0, 0.0], [0.0, 0.0]], objects))
        self.assertEqual(report["lfe"]["result"], "ZERO_LFE_INPUT_AND_NO_LFE_TERM_IN_FORMULA")
        self.assertEqual(report["totals"]["actualToIncoherentRatio"], 1.0)

    def test_coherent_same_phase_and_known_gain(self):
        objects = [[self._obj(4.0, 2.0, 4.0, 2.0, 1.0)], [self._obj(0, 0, 0, 0)]]
        report = analyze(self._trace([[1.0, 1.0], [0.0, 0.0]], objects))
        self.assertEqual(report["totals"]["crossTermEnergy"], 2.0)
        self.assertEqual(report["totals"]["actualToIncoherentRatio"], 2.0)

    def test_known_matrix_gain(self):
        objects = [[self._obj(4.0, 4.0, 4.0, 0.0, 2.0)], [self._obj(0, 0, 0, 0)]]
        report = analyze(self._trace([[1.0, 0.0], [0.0, 0.0]], objects))
        self.assertEqual(report["totals"]["maxMatrixRowNorm"], 2.0)
        self.assertEqual(report["totals"]["actualToIncoherentRatio"], 1.0)

    def test_anti_phase_cancels_but_baseline_remains(self):
        objects = [[self._obj(0.0, 2.0, 0.0, -2.0)], [self._obj(0, 0, 0, 0)]]
        report = analyze(self._trace([[1.0, 1.0], [0.0, 0.0]], objects))
        self.assertEqual(report["totals"]["objectActualEnergy"], 0.0)
        self.assertEqual(report["totals"]["objectIncoherentBaselineEnergy"], 2.0)

    def test_nonzero_lfe_is_separate(self):
        objects = [[self._obj(1.0, 1.0, 1.0, 0.0)], [self._obj(0, 0, 0, 0)]]
        report = analyze(self._trace([[1.0, 0.0], [0.0, 0.0]], objects, lfe=3.0))
        self.assertEqual(report["lfe"]["result"],
                         "NONZERO_LFE_EXCLUDED_FROM_TRACED_QMF_RECONSTRUCTION")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path, nargs="?")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        result = unittest.TextTestRunner(verbosity=1).run(
            unittest.defaultTestLoader.loadTestsFromTestCase(LowFrequencyAuditTests))
        raise SystemExit(0 if result.wasSuccessful() else 1)
    if args.trace is None or args.output_dir is None:
        parser.error("trace and --output-dir are required unless --self-test is used")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    report = analyze(args.trace)
    json_path = args.output_dir / "joc-low-frequency-audit.json"
    md_path = args.output_dir / "joc-low-frequency-audit.md"
    json_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    md_path.write_text(markdown(report), encoding="utf-8")
    print(json.dumps({"result": report["result"], "json": str(json_path.resolve()),
                      "markdown": str(md_path.resolve())}))


if __name__ == "__main__":
    main()
