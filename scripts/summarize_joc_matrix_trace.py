#!/usr/bin/env python3
"""Create a compact summary from a joc-matrix-trace-v1 JSONL file."""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
from typing import Any


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _finite(value: Any) -> bool:
    return isinstance(value, (int, float)) and math.isfinite(float(value))


def _load(path: Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    with path.open("r", encoding="utf-8") as stream:
        records = [json.loads(line) for line in stream if line.strip()]
    if not records or records[0].get("recordType") != "provenance":
        raise ValueError("missing provenance record")
    if records[0].get("schema") != "joc-matrix-trace-v1":
        raise ValueError("unsupported trace schema")
    aus = records[1:]
    if not aus or any(record.get("recordType") != "au" for record in aus):
        raise ValueError("trace has no AU records or contains a non-AU record")
    return records[0], aus


def _int(value: Any, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{label} must be an integer")
    return value


def _range(value: Any, limit: int, label: str) -> tuple[int, int]:
    if not isinstance(value, list) or len(value) != 2:
        raise ValueError(f"{label} must be [start,end]")
    start = _int(value[0], f"{label}.start")
    end = _int(value[1], f"{label}.end")
    if start < 0 or end < start or end > limit:
        raise ValueError(f"{label} out of bounds")
    return start, end


def _optional_range(item: dict[str, Any], start_key: str, end_key: str,
                    limit: int, label: str) -> tuple[int, int] | None:
    start = item.get(start_key)
    end = item.get(end_key)
    if start is None or end is None:
        if start is not None or end is not None:
            raise ValueError(f"{label} must use null for both endpoints")
        return None
    start_int = _int(start, f"{label}.start")
    end_int = _int(end, f"{label}.end")
    if start_int < 0 or end_int < start_int or end_int > limit:
        raise ValueError(f"{label} out of bounds")
    return start_int, end_int


def _validate_forensic_au(au: dict[str, Any]) -> dict[str, Any]:
    """Validate one forensic AU and independently rebuild its q cells."""
    payload_bits = _int(au.get("payloadBitCount"), "payloadBitCount")
    syntax_end = _int(au.get("syntaxBitEnd"), "syntaxBitEnd")
    if payload_bits <= 0 or syntax_end < 0 or syntax_end > payload_bits:
        raise ValueError("invalid payload/syntax bit bounds")
    payload_hex = au.get("payloadHex")
    if not isinstance(payload_hex, str) or not payload_hex \
            or len(payload_hex) % 2 \
            or any(character not in "0123456789abcdefABCDEF" for character in payload_hex):
        raise ValueError("invalid forensic payloadHex")
    payload = bytes.fromhex(payload_hex)
    if len(payload) * 8 != payload_bits:
        raise ValueError("payloadHex length disagrees with payloadBitCount")

    def payload_codeword(offset: int, length: int) -> str:
        if offset < 0 or length <= 0 or offset + length > syntax_end:
            raise ValueError("forensic codeword exceeds syntax bits")
        return "".join(
            "1" if (payload[bit // 8] >> (7 - bit % 8)) & 1 else "0"
            for bit in range(offset, offset + length)
        )

    num_channels = _int(au.get("numChannels"), "numChannels")
    num_objects = _int(au.get("numObjects"), "numObjects")
    if num_channels <= 0 or num_objects <= 0:
        raise ValueError("invalid forensic channel/object count")
    objects = au.get("objects")
    if not isinstance(objects, list) or len(objects) != num_objects:
        raise ValueError("forensic object count mismatch")

    stats: dict[str, Any] = {
        "payloadBitCount": payload_bits,
        "syntaxBitEnd": syntax_end,
        "objectHeaderRangeCount": 0,
        "objectDataRangeCount": 0,
        "offsetTsRangeCount": 0,
        "symbolDataRangeCount": 0,
        "symbolCount": 0,
        "symbolKindCounts": Counter(),
        "checkedCellCount": 0,
        "qMismatchCount": 0,
    }
    previous_header_end: int | None = None
    data_ranges: list[tuple[int, int]] = []

    for object_index, item in enumerate(objects):
        if not isinstance(item, dict):
            raise ValueError(f"object {object_index} is not an object")
        forensic = item.get("forensic")
        if not isinstance(forensic, dict):
            raise ValueError(f"object {object_index} missing forensic record")
        header = _range([
            forensic.get("headerBitOffset"), forensic.get("headerBitEnd")
        ], syntax_end, f"object {object_index} header")
        if previous_header_end is not None and header[0] != previous_header_end:
            raise ValueError("object header ranges are not ordered")
        previous_header_end = header[1]
        stats["objectHeaderRangeCount"] += 1

        present = item.get("present") is True
        num_bands = _int(item.get("numBands"), f"object {object_index}.numBands")
        num_data_points = _int(item.get("numDataPoints"),
                               f"object {object_index}.numDataPoints")
        quant_index = _int(item.get("quantIndex"), f"object {object_index}.quantIndex")
        if quant_index not in (0, 1):
            raise ValueError(f"object {object_index} quantIndex is not 0/1")
        quant_steps = 96 if quant_index == 0 else 192
        data_range = _optional_range(
            forensic, "dataBitOffset", "dataBitEnd", syntax_end,
            f"object {object_index} data")
        data_points = forensic.get("dataPoints")
        if not isinstance(data_points, list):
            raise ValueError(f"object {object_index} dataPoints is not an array")
        if not present:
            if num_bands != 0 or num_data_points != 0 \
                    or item.get("slopeIndex") != 0 \
                    or data_range is not None or data_points:
                raise ValueError(f"absent object {object_index} has data ranges")
            if item.get("q") not in ([], None):
                raise ValueError(f"absent object {object_index} has q cells")
            continue
        if num_bands <= 0 or num_data_points <= 0:
            raise ValueError(f"object {object_index} has invalid dimensions")
        if data_range is None or len(data_points) != num_data_points:
            raise ValueError(f"object {object_index} data range/count mismatch")
        data_ranges.append(data_range)
        stats["objectDataRangeCount"] += 1
        sparse = item.get("sparse") is True
        expected_kinds = (["sparse-fixed-channel"]
                          + ["sparse-huffman-channel-delta"] * (num_bands - 1)
                          + ["huffman-coefficient"] * num_bands) if sparse else (
                              ["huffman-coefficient"] * (num_channels * num_bands))
        previous_dp_end = data_range[0]
        coefficients: list[list[dict[str, Any]]] = []
        for dp_index, data_point in enumerate(data_points):
            if not isinstance(data_point, dict):
                raise ValueError(f"object {object_index} dp {dp_index} is not an object")
            offset_range = _optional_range(
                data_point, "offsetTsBitOffset", "offsetTsBitEnd", header[1],
                f"object {object_index} dp {dp_index} offsetTs")
            if item.get("slopeIndex") == 1:
                if offset_range is None or offset_range[0] < header[0]:
                    raise ValueError("slope-1 offsetTs range is missing/outside header")
                stats["offsetTsRangeCount"] += 1
            elif offset_range is not None:
                raise ValueError("slope-0 data point has offsetTs range")
            symbol_range = _optional_range(
                data_point, "symbolDataBitOffset", "symbolDataBitEnd", syntax_end,
                f"object {object_index} dp {dp_index} symbols")
            if symbol_range is None or symbol_range[0] < data_range[0] \
                    or symbol_range[1] > data_range[1] \
                    or symbol_range[0] != previous_dp_end:
                raise ValueError("data-point symbol range is outside/overlapping object data")
            previous_dp_end = symbol_range[1]
            stats["symbolDataRangeCount"] += 1
            symbols = data_point.get("symbols")
            if not isinstance(symbols, list) or len(symbols) != len(expected_kinds):
                raise ValueError(f"object {object_index} dp {dp_index} symbol count mismatch")
            previous_symbol_end = symbol_range[0]
            dp_coefficients: list[dict[str, Any]] = []
            for symbol_index, symbol in enumerate(symbols):
                if not isinstance(symbol, dict):
                    raise ValueError("forensic symbol is not an object")
                kind = symbol.get("kind")
                if kind != expected_kinds[symbol_index]:
                    raise ValueError("forensic symbol kind/order mismatch")
                offset = _int(symbol.get("bitOffset"), "symbol bitOffset")
                length = _int(symbol.get("bitLength"), "symbol bitLength")
                codeword = symbol.get("codeword")
                if length <= 0 or not isinstance(codeword, str) \
                        or len(codeword) != length \
                        or any(bit not in "01" for bit in codeword):
                    raise ValueError("invalid forensic codeword/range")
                end = offset + length
                if offset < symbol_range[0] or end > symbol_range[1] \
                        or offset != previous_symbol_end:
                    raise ValueError("forensic symbols are outside/overlapping symbol data")
                previous_symbol_end = end
                if payload_codeword(offset, length) != codeword:
                    raise ValueError("forensic codeword disagrees with payloadHex")
                raw = _int(symbol.get("symbol"), "symbol value")
                parameter_band = _int(symbol.get("parameterBand"),
                                      "symbol parameterBand")
                input_channel = symbol.get("inputChannel")
                resolved_channel = symbol.get("resolvedInputChannel")
                if parameter_band < 0 or parameter_band >= num_bands:
                    raise ValueError("symbol parameterBand out of bounds")
                if kind == "sparse-fixed-channel":
                    if parameter_band != 0 or symbol_index != 0 \
                            or input_channel is not None \
                            or not isinstance(resolved_channel, int) \
                            or isinstance(resolved_channel, bool) \
                            or not 0 <= resolved_channel < num_channels \
                            or not 0 <= raw < num_channels:
                        raise ValueError("invalid sparse fixed-channel coordinates")
                elif kind == "sparse-huffman-channel-delta":
                    if parameter_band != symbol_index or input_channel is not None \
                            or not isinstance(resolved_channel, int) \
                            or isinstance(resolved_channel, bool) \
                            or not 0 <= resolved_channel < num_channels \
                            or not 0 <= raw < num_channels:
                        raise ValueError("invalid sparse channel-delta coordinates")
                else:
                    expected_coefficient_index = (symbol_index - num_bands
                                                  if sparse else symbol_index)
                    expected_parameter_band = expected_coefficient_index % num_bands
                    if parameter_band != expected_parameter_band:
                        raise ValueError("coefficient parameterBand/order mismatch")
                    if not 0 <= raw < quant_steps:
                        raise ValueError("raw coefficient outside quantizer")
                    if sparse:
                        if input_channel is not None \
                                or not isinstance(resolved_channel, int) \
                                or isinstance(resolved_channel, bool) \
                                or not 0 <= resolved_channel < num_channels:
                            raise ValueError("invalid sparse coefficient coordinates")
                    else:
                        expected_input_channel = expected_coefficient_index // num_bands
                        if resolved_channel is not None \
                                or not isinstance(input_channel, int) \
                                or isinstance(input_channel, bool) \
                                or input_channel != expected_input_channel \
                                or not 0 <= input_channel < num_channels:
                            raise ValueError("invalid dense coefficient coordinates")
                    dp_coefficients.append(symbol)
                stats["symbolCount"] += 1
                stats["symbolKindCounts"][kind] += 1
            coefficients.append(dp_coefficients)
        if previous_dp_end != data_range[1]:
            raise ValueError("data point ranges exceed object data range")

        q = item.get("q")
        if not isinstance(q, list) or len(q) != num_data_points:
            raise ValueError(f"object {object_index} q shape mismatch")
        expected_q = [
            [[50 if quant_index == 0 else 100 for _ in range(num_bands)]
             for _ in range(num_channels)]
            for _ in range(num_data_points)
        ]
        if sparse:
            channel_mod = item.get("channelIndexMod")
            if not isinstance(channel_mod, list) or len(channel_mod) != num_data_points:
                raise ValueError("sparse channelIndexMod shape mismatch")
            for dp_index, dp_symbols in enumerate(coefficients):
                channel_symbols = [
                    symbol for symbol in data_points[dp_index]["symbols"]
                    if symbol["kind"] in ("sparse-fixed-channel",
                                           "sparse-huffman-channel-delta")
                ]
                if len(channel_symbols) != num_bands:
                    raise ValueError("sparse channel symbol count mismatch")
                expected_mod = []
                for channel_band, symbol in enumerate(channel_symbols):
                    raw_channel = symbol["symbol"]
                    resolved = raw_channel if channel_band == 0 else (
                        channel_symbols[channel_band - 1]["symbol"] + raw_channel
                    ) % num_channels
                    if symbol["resolvedInputChannel"] != resolved:
                        raise ValueError("sparse resolved channel disagrees with raw deltas")
                    expected_mod.append(resolved)
                actual_mod = channel_mod[dp_index]
                if actual_mod != expected_mod:
                    raise ValueError("sparse channelIndexMod disagrees with forensic symbols")
                for symbol in dp_symbols:
                    band = symbol["parameterBand"]
                    channel = symbol["resolvedInputChannel"]
                    previous = expected_q[dp_index][channel][band - 1] if band > 0 else None
                    value = (50 if quant_index == 0 else 100) + symbol["symbol"] \
                        if band == 0 else previous + symbol["symbol"]
                    expected_q[dp_index][channel][band] = value % quant_steps
        else:
            for dp_index, dp_symbols in enumerate(coefficients):
                for symbol in dp_symbols:
                    channel = symbol["inputChannel"]
                    band = symbol["parameterBand"]
                    previous = expected_q[dp_index][channel][band - 1] if band > 0 else None
                    value = (48 if quant_index == 0 else 96) + symbol["symbol"] \
                        if band == 0 else previous + symbol["symbol"]
                    expected_q[dp_index][channel][band] = value % quant_steps
        for dp_index, row in enumerate(q):
            if not isinstance(row, list) or len(row) != num_channels:
                raise ValueError("q channel shape mismatch")
            for channel in range(num_channels):
                if not isinstance(row[channel], list) or len(row[channel]) != num_bands:
                    raise ValueError("q parameter-band shape mismatch")
                for band in range(num_bands):
                    actual = _int(row[channel][band], "q cell")
                    if actual != expected_q[dp_index][channel][band]:
                        stats["qMismatchCount"] += 1
        stats["checkedCellCount"] += num_data_points * num_channels * num_bands

    if previous_header_end is None:
        raise ValueError("forensic trace has no object headers")
    if data_ranges:
        if data_ranges[0][0] != previous_header_end:
            raise ValueError("first object data does not follow final object header")
        for previous, current in zip(data_ranges, data_ranges[1:]):
            if current[0] != previous[1]:
                raise ValueError("object data ranges are not contiguous")
        if data_ranges[-1][1] != syntax_end:
            raise ValueError("final object data does not reach syntaxBitEnd")
    elif syntax_end != previous_header_end:
        raise ValueError("syntax has unaccounted bits after object headers")
    if stats["qMismatchCount"]:
        raise ValueError(
            "forensic q reconstruction mismatch: "
            f"checkedCellCount={stats['checkedCellCount']} "
            f"mismatchCount={stats['qMismatchCount']}"
        )
    stats["symbolKindCounts"] = dict(stats["symbolKindCounts"])
    stats["rangeCount"] = (stats["objectHeaderRangeCount"]
                            + stats["objectDataRangeCount"]
                            + stats["offsetTsRangeCount"]
                            + stats["symbolDataRangeCount"])
    return stats


def summarize(path: Path) -> dict[str, Any]:
    provenance, aus = _load(path)
    first = aus[0]
    identities = first.get("inputIdentities")
    if not isinstance(identities, list) or not all(isinstance(v, str) for v in identities):
        raise ValueError("invalid input identity order")
    qin_total = [0.0] * len(identities)
    qin_max = [0.0] * len(identities)
    object_count = int(first.get("numObjects", 0))
    output_total = [0.0] * object_count
    output_max = [0.0] * object_count
    contribution_total = [[0.0] * len(identities) for _ in range(object_count)]
    max_abs = 0.0
    max_relative = 0.0
    mismatch_count = 0
    compared_count = 0
    state_resets = 0
    discontinuities = 0
    present_count = [0] * object_count
    absent_count = [0] * object_count
    transition_count = [0] * object_count
    forensic_contract = bool(provenance.get("forensicContract"))
    forensic_enabled = forensic_contract
    forensic_totals: dict[str, Any] = {
        "checkedCellCount": 0,
        "qMismatchCount": 0,
        "symbolCount": 0,
        "rangeCount": 0,
        "objectHeaderRangeCount": 0,
        "objectDataRangeCount": 0,
        "offsetTsRangeCount": 0,
        "symbolDataRangeCount": 0,
        "symbolKindCounts": Counter(),
    }

    for au in aus:
        if int(au.get("numObjects", -1)) != object_count:
            raise ValueError("object count changed within trace")
        qin = au.get("qin", [])
        objects = au.get("objects", [])
        transitions = au.get("becamePresentAfterAbsent", [])
        check = au.get("reconstructionCheck", {})
        if (len(qin) != len(identities) or len(objects) != object_count
                or len(transitions) != object_count
                or not all(isinstance(value, bool) for value in transitions)):
            raise ValueError("trace shape mismatch")
        forensic_flags = [isinstance(item, dict) and "forensic" in item
                          for item in objects]
        if any(forensic_flags):
            forensic_enabled = True
            if not all(forensic_flags):
                raise ValueError("forensic trace is incomplete within AU")
            if _int(au.get("payloadBitCount"), "payloadBitCount") <= 0:
                raise ValueError("forensic payload bit count is missing")
            au_forensic = _validate_forensic_au(au)
            for key in ("checkedCellCount", "qMismatchCount", "symbolCount",
                        "rangeCount", "objectHeaderRangeCount",
                        "objectDataRangeCount", "offsetTsRangeCount",
                        "symbolDataRangeCount"):
                forensic_totals[key] += au_forensic[key]
            forensic_totals["symbolKindCounts"].update(
                au_forensic["symbolKindCounts"])
        elif forensic_contract:
            raise ValueError("forensic contract is present but AU has no forensic data")
        for channel, item in enumerate(qin):
            energy = item.get("energy")
            if not _finite(energy):
                raise ValueError("non-finite Qin energy")
            qin_total[channel] += float(energy)
            qin_max[channel] = max(qin_max[channel], float(energy))
        for obj_index, item in enumerate(objects):
            present = item.get("present") is True
            if present:
                present_count[obj_index] += 1
            else:
                absent_count[obj_index] += 1
            if transitions[obj_index] is True:
                transition_count[obj_index] += 1
            output = item.get("qoutEnergy")
            if not _finite(output):
                raise ValueError("non-finite Qout energy")
            output_total[obj_index] += float(output)
            output_max[obj_index] = max(output_max[obj_index], float(output))
            contributions = item.get("contributions", [])
            if len(contributions) != len(identities):
                raise ValueError("contribution shape mismatch")
            for channel, contribution in enumerate(contributions):
                energy = contribution.get("energy")
                if not _finite(energy):
                    raise ValueError("non-finite contribution energy")
                contribution_total[obj_index][channel] += float(energy)
        if not _finite(check.get("maxAbs")) or not _finite(check.get("relativeRms")):
            raise ValueError("non-finite reconstruction check")
        max_abs = max(max_abs, float(check["maxAbs"]))
        max_relative = max(max_relative, float(check["relativeRms"]))
        mismatch_count += int(check.get("mismatchCount", 0))
        compared_count += int(check.get("comparedCount", 0))
        state_resets += int(au.get("stateReset") is True)
        discontinuities += int(au.get("sequenceDiscontinuity") is True)

    object_summaries: list[dict[str, Any]] = []
    all_contributions = [energy for row in contribution_total for energy in row]
    total_contribution = sum(all_contributions)
    for obj_index, row in enumerate(contribution_total):
        order = sorted(range(len(row)), key=lambda channel: row[channel], reverse=True)
        top = [
            {
                "identity": identities[channel],
                "energy": row[channel],
                "fraction": row[channel] / sum(row) if sum(row) else 0.0,
            }
            for channel in order[:3]
        ]
        object_summaries.append({
            "object": obj_index,
            "presentAUs": present_count[obj_index],
            "absentAUs": absent_count[obj_index],
            "becamePresentAfterAbsent": transition_count[obj_index],
            "qoutEnergyTotal": output_total[obj_index],
            "qoutEnergyMax": output_max[obj_index],
            "topContributions": top,
        })

    nonzero = sum(energy > 1.0e-20 for energy in all_contributions)
    top_fraction = max(all_contributions, default=0.0) / total_contribution \
        if total_contribution else 0.0
    if not total_contribution:
        classification = "zero"
    elif top_fraction >= 0.9:
        classification = "concentrated"
    else:
        classification = "distributed"
    qout_total = sum(output_total)
    qout_order = sorted(range(object_count), key=lambda obj: output_total[obj], reverse=True)
    qout_nonzero = sum(energy > 1.0e-20 for energy in output_total)
    qout_top = qout_order[0] if qout_order else None
    qout_top_fraction = (output_total[qout_top] / qout_total
                         if qout_top is not None and qout_total else 0.0)
    return {
        "recordType": "summary",
        "schema": "joc-matrix-summary-v1",
        "tracePath": str(path),
        "traceSha256": _sha256(path),
        "sourcePath": provenance.get("sourcePath"),
        "sourceFileDigest": provenance.get("sourceFileDigest"),
        "qwinCoefficientCount": provenance.get("qwinCoefficientCount"),
        "qwinDigest": provenance.get("qwinDigest"),
        "config": first.get("config"),
        "numChannels": len(identities),
        "numObjects": object_count,
        "auCount": len(aus),
        "inputIdentities": identities,
        "qin": [
            {"identity": identities[channel], "energyTotal": qin_total[channel],
             "energyMax": qin_max[channel]}
            for channel in range(len(identities))
        ],
        "reconstruction": {
            "maxAbs": max_abs,
            "maxRelativeRms": max_relative,
            "mismatchCount": mismatch_count,
            "comparedCount": compared_count,
        },
        "stateResets": state_resets,
        "sequenceDiscontinuities": discontinuities,
        "objects": object_summaries,
        "matrixContribution": {
            "classification": classification,
            "nonzeroContributions": nonzero,
            "totalEnergy": total_contribution,
            "topCellFraction": top_fraction,
        },
        "qoutDistribution": {
            "totalEnergy": qout_total,
            "nonzeroObjectCount": qout_nonzero,
            "topObject": qout_top,
            "topObjectFraction": qout_top_fraction,
            "diagnosticNonzeroThreshold": 1.0e-20,
        },
        "absentPolicy": first.get("absentPolicy"),
        "forensicAudit": {
            "enabled": forensic_enabled,
            **{key: value for key, value in forensic_totals.items()
               if key != "symbolKindCounts"},
            "symbolKindCounts": dict(forensic_totals["symbolKindCounts"]),
        },
    }


def _self_test() -> None:
    import tempfile

    provenance = {
        "recordType": "provenance", "schema": "joc-matrix-trace-v1",
        "sourcePath": "fixture.ec3", "sourceFileDigest": "fnv1a64-test",
        "qwinCoefficientCount": 1, "qwinDigest": "fnv1a64-qwin",
    }
    absent_au = {
        "recordType": "au", "config": 3, "numChannels": 1, "numObjects": 1,
        "stateReset": True, "sequenceDiscontinuity": False,
        "inputIdentities": ["FL"], "qin": [{"energy": 2.0}],
        "objects": [{"present": False, "qoutEnergy": 0.0,
                      "contributions": [{"energy": 1.0}]}],
        "becamePresentAfterAbsent": [False],
        "reconstructionCheck": {"maxAbs": 0.0, "relativeRms": 0.0,
                                 "mismatchCount": 0, "comparedCount": 1},
        "absentPolicy": "zero-output-and-clear-previous-state",
    }
    present_au = dict(absent_au)
    present_au["stateReset"] = False
    present_au["objects"] = [{"present": True, "qoutEnergy": 1.0,
                               "contributions": [{"energy": 1.0}]}]
    present_au["becamePresentAfterAbsent"] = [True]
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "trace.jsonl"
        path.write_text(json.dumps(provenance) + "\n"
                        + json.dumps(absent_au) + "\n"
                        + json.dumps(present_au) + "\n",
                        encoding="utf-8")
        result = summarize(path)
        assert result["auCount"] == 2
        assert result["reconstruction"]["mismatchCount"] == 0
        assert result["matrixContribution"]["classification"] == "concentrated"
        assert result["objects"][0]["absentAUs"] == 1
        assert result["objects"][0]["becamePresentAfterAbsent"] == 1
        assert result["qoutDistribution"]["nonzeroObjectCount"] == 1
        assert result["qoutDistribution"]["topObject"] == 0

    def forensic_fixture(sparse: bool) -> dict[str, Any]:
        if sparse:
            symbols = [
                {"kind": "sparse-fixed-channel", "bitOffset": 30,
                 "bitLength": 1, "codeword": "0", "symbol": 1,
                 "parameterBand": 0, "inputChannel": None,
                 "resolvedInputChannel": 1},
                {"kind": "sparse-huffman-channel-delta", "bitOffset": 31,
                 "bitLength": 1, "codeword": "0", "symbol": 1,
                 "parameterBand": 1, "inputChannel": None,
                 "resolvedInputChannel": 0},
                {"kind": "sparse-huffman-channel-delta", "bitOffset": 32,
                 "bitLength": 1, "codeword": "0", "symbol": 0,
                 "parameterBand": 2, "inputChannel": None,
                 "resolvedInputChannel": 1},
                {"kind": "huffman-coefficient", "bitOffset": 33,
                 "bitLength": 1, "codeword": "0", "symbol": 5,
                 "parameterBand": 0, "inputChannel": None,
                 "resolvedInputChannel": 1},
                {"kind": "huffman-coefficient", "bitOffset": 34,
                 "bitLength": 1, "codeword": "0", "symbol": 7,
                 "parameterBand": 1, "inputChannel": None,
                 "resolvedInputChannel": 0},
                {"kind": "huffman-coefficient", "bitOffset": 35,
                 "bitLength": 1, "codeword": "0", "symbol": 11,
                 "parameterBand": 2, "inputChannel": None,
                 "resolvedInputChannel": 1},
            ]
            q = [[[50, 57, 50], [55, 50, 61]]]
            channel_mod = [[1, 0, 1]]
            bands = 3
        else:
            symbols = []
            for channel, raw_values in enumerate(((1, 2), (4, 2))):
                for band, raw in enumerate(raw_values):
                    symbols.append({
                        "kind": "huffman-coefficient", "bitOffset": 30 + len(symbols),
                        "bitLength": 1, "codeword": "0", "symbol": raw,
                        "parameterBand": band, "inputChannel": channel,
                        "resolvedInputChannel": None,
                    })
            q = [[[49, 51], [52, 54]]]
            channel_mod = None
            bands = 2
        obj = {
            "object": 0, "present": True, "sparse": sparse,
            "numBands": bands, "quantIndex": 0, "slopeIndex": 0,
            "numDataPoints": 1, "q": q,
            "forensic": {
                "headerBitOffset": 10, "headerBitEnd": 30,
                "dataBitOffset": 30, "dataBitEnd": 30 + len(symbols),
                "dataPoints": [{
                    "offsetTsBitOffset": None, "offsetTsBitEnd": None,
                    "symbolDataBitOffset": 30,
                    "symbolDataBitEnd": 30 + len(symbols),
                    "symbols": symbols,
                }],
            },
        }
        if channel_mod is not None:
            obj["channelIndexMod"] = channel_mod
        return {
            "recordType": "au", "numChannels": 2, "numObjects": 1,
            "objects": [obj], "payloadBitCount": 64,
            "syntaxBitEnd": 30 + len(symbols),
            "payloadHex": "00" * 8,
        }

    dense_forensic = forensic_fixture(False)
    sparse_forensic = forensic_fixture(True)
    dense_stats = _validate_forensic_au(dense_forensic)
    sparse_stats = _validate_forensic_au(sparse_forensic)
    assert dense_stats["checkedCellCount"] == 4
    assert sparse_stats["checkedCellCount"] == 6
    assert sparse_stats["symbolKindCounts"]["sparse-huffman-channel-delta"] == 2
    absent_forensic = {
        "recordType": "au", "numChannels": 2, "numObjects": 2,
        "payloadBitCount": 64, "payloadHex": "00" * 8, "syntaxBitEnd": 12,
        "objects": [
            {"present": False, "numBands": 0, "quantIndex": 0,
             "slopeIndex": 0, "numDataPoints": 0, "q": [],
             "forensic": {"headerBitOffset": 10, "headerBitEnd": 11,
                           "dataBitOffset": None, "dataBitEnd": None,
                           "dataPoints": []}},
            {"present": False, "numBands": 0, "quantIndex": 1,
             "slopeIndex": 0, "numDataPoints": 0, "q": [],
             "forensic": {"headerBitOffset": 11, "headerBitEnd": 12,
                           "dataBitOffset": None, "dataBitEnd": None,
                           "dataPoints": []}},
        ],
    }
    assert _validate_forensic_au(absent_forensic)["checkedCellCount"] == 0
    tampered_raw = json.loads(json.dumps(sparse_forensic))
    tampered_raw["objects"][0]["forensic"]["dataPoints"][0]["symbols"][3]["symbol"] = 6
    try:
        _validate_forensic_au(tampered_raw)
    except ValueError as error:
        assert "q reconstruction mismatch" in str(error)
    else:
        raise AssertionError("tampered raw symbol was accepted")
    tampered_range = json.loads(json.dumps(dense_forensic))
    tampered_range["objects"][0]["forensic"]["dataPoints"][0]["symbols"][0]["bitOffset"] = 29
    try:
        _validate_forensic_au(tampered_range)
    except ValueError:
        pass
    else:
        raise AssertionError("tampered forensic range was accepted")
    tampered_gap = json.loads(json.dumps(dense_forensic))
    tampered_gap["objects"][0]["forensic"]["dataPoints"][0]["symbols"][1]["bitOffset"] = 32
    try:
        _validate_forensic_au(tampered_gap)
    except ValueError:
        pass
    else:
        raise AssertionError("gapped forensic symbols were accepted")
    tampered_payload = json.loads(json.dumps(dense_forensic))
    payload_bytes = bytearray.fromhex(tampered_payload["payloadHex"])
    payload_bytes[30 // 8] |= 1 << (7 - (30 % 8))
    tampered_payload["payloadHex"] = payload_bytes.hex()
    try:
        _validate_forensic_au(tampered_payload)
    except ValueError as error:
        assert "codeword disagrees" in str(error)
    else:
        raise AssertionError("tampered payload bit was accepted")
    print("summarize_joc_matrix_trace self-test: PASS")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        _self_test()
        return 0
    if args.input is None or args.output is None:
        parser.error("--input and --output are required unless --self-test is used")
    result = summarize(args.input)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n",
                           encoding="utf-8")
    print(f"summary:PASS path={args.output} auCount={result['auCount']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
