"""Render ADM/BWF to a 22-channel BS.2051 System-H f32 bus with EAR.

This helper is deliberately a thin offline adapter.  It uses the pinned EAR
2.1.0 renderer for DirectSpeakers and Objects, preserves the two ADM LFE
layout sidecar channels, and never touches the project playback path.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
import time
from pathlib import Path

import numpy as np


RATE = 48000
BLOCK = 8192


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def runtime_paths(reference_root: Path) -> None:
    ear_root = reference_root / "ear-2.1.0" / "ebu_adm_renderer-2.1.0"
    if not ear_root.is_dir():
        raise RuntimeError(f"missing pinned EAR source: {ear_root}")
    sys.path.insert(0, str(ear_root.resolve()))


def repair_logic_stream_graph(adm) -> dict:
    repaired = 0
    for stream in adm.audioStreamFormats:
        if stream.audioPackFormat is not None and stream.audioChannelFormat is not None:
            # EAR 2.1.0 rejects this Logic-produced dual reference.  The
            # channel format remains authoritative for the selected track.
            stream.audioPackFormat = None
            repaired += 1
    return {
        "audioStreamFormatsClearedPackReference": repaired,
    }


def repair_selected_lfe_labels(adm, selected) -> dict:
    """Map only selected room-centric LFE tracks to EAR's LFE slots."""
    # Logic uses the room-centric label RC_LFE. EAR's DirectSpeakers
    # compatibility table recognises the normative LFE1/LFE2 labels; retain
    # the channel position but make this selected-track-only repair so LFE
    # lands on the explicit sideband instead of a nearest main speaker.
    direct_lfe_tracks = []
    lfe_blocks = 0
    next_slot = 1
    for item in selected:
        if type(item).__name__ != "DirectSpeakersRenderingItem":
            continue
        track_index = item.track_spec.track_index
        track_uid = next(
            (uid for uid in adm.audioTrackUIDs if uid.trackIndex == track_index + 1),
            None,
        )
        pack = getattr(track_uid, "audioPackFormat", None)
        channel_formats = list(getattr(pack, "audioChannelFormats", []) or [])
        if track_index >= len(channel_formats):
            continue
        channel = channel_formats[track_index]
        labels = [
            label
            for block in channel.audioBlockFormats
            for label in (getattr(block, "speakerLabel", []) or [])
        ]
        if not any("LFE" in label.upper() for label in labels):
            continue
        direct_lfe_tracks.append(track_index)
        slot = f"LFE{min(next_slot, 2)}"
        next_slot += 1
        for block in channel.audioBlockFormats:
            block_labels = list(getattr(block, "speakerLabel", []) or [])
            if any("LFE" in label.upper() for label in block_labels) and not any(
                label.upper() in ("LFE1", "LFE2") for label in block_labels
            ):
                block.speakerLabel = [
                    slot if "LFE" in label.upper() else label for label in block_labels
                ]
                lfe_blocks += 1
    return {
        "selectedDirectSpeakerLfeTracks": direct_lfe_tracks,
        "roomCentricLfeBlocksMappedToLfeSlots": lfe_blocks,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--adm", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--reference-root", type=Path, required=True)
    parser.add_argument("--start-sec", type=float, default=0.0)
    parser.add_argument("--end-sec", type=float, required=True)
    args = parser.parse_args()

    if not args.adm.is_file():
        raise RuntimeError(f"ADM file does not exist: {args.adm}")
    if not (0.0 <= args.start_sec < args.end_sec):
        raise RuntimeError("require 0 <= start-sec < end-sec")
    args.out_dir.mkdir(parents=True, exist_ok=True)
    main_raw = args.out_dir / "ear-system-h-22ch-f32.raw"
    lfe_raw = args.out_dir / "ear-system-h-lfe-2ch-f32.raw"
    if main_raw.exists() or lfe_raw.exists():
        raise RuntimeError("refusing to overwrite existing EAR output")

    runtime_paths(args.reference_root)
    from ear.core import Renderer, bs2051
    from ear.core.metadata_processing import preprocess_rendering_items
    from ear.core.select_items.select_items import select_rendering_items
    from ear.fileio.adm import timing_fixes
    from ear.fileio.utils import openBw64Adm

    start_frame = int(round(args.start_sec * RATE))
    end_frame = int(round(args.end_sec * RATE))
    output_cursor = 0
    rendered_input = 0
    selected_main = 0
    selected_lfe = 0
    input_channels = None
    start_time = time.monotonic()
    main_handle = main_raw.open("wb")
    lfe_handle = lfe_raw.open("wb")
    try:
        with openBw64Adm(str(args.adm)) as infile:
            if infile.sampleRate != RATE:
                raise RuntimeError(f"ADM sample rate {infile.sampleRate}, expected {RATE}")
            if infile.channels <= 0:
                raise RuntimeError(f"ADM has invalid channel count: {infile.channels}")
            input_channels = infile.channels
            repairs = repair_logic_stream_graph(infile.adm)
            # Logic's AXML contains adjacent blocks whose declared durations
            # do not meet the next rtime. EAR's documented timing repair is
            # used in-memory only; it never rewrites the source ADM.
            timing_fixes.fix_blockFormat_timings(infile.adm)
            selected = preprocess_rendering_items(select_rendering_items(infile.adm))
            repairs.update(repair_selected_lfe_labels(infile.adm, selected))
            counts = {
                "DirectSpeakers": sum(type(item).__name__ == "DirectSpeakersRenderingItem" for item in selected),
                "Objects": sum(type(item).__name__ == "ObjectRenderingItem" for item in selected),
                "HOA": sum(type(item).__name__ == "HOARenderingItem" for item in selected),
            }
            layout = bs2051.get_layout("9+10+3")
            main_indices = [i for i, is_lfe in enumerate(layout.is_lfe) if not is_lfe]
            lfe_indices = [i for i, is_lfe in enumerate(layout.is_lfe) if is_lfe]
            if len(layout.channels) != 24 or len(main_indices) != 22 or len(lfe_indices) != 2:
                raise RuntimeError("EAR 9+10+3 layout is not 22 main + 2 LFE channels")
            renderer = Renderer(layout)
            renderer.set_rendering_items(selected)

            def emit(block) -> None:
                nonlocal output_cursor, selected_main, selected_lfe
                array = np.asarray(block, dtype=np.float32)
                if array.ndim != 2 or array.shape[1] != 24:
                    raise RuntimeError(f"bad EAR output shape {array.shape}")
                if not np.isfinite(array).all():
                    raise RuntimeError("EAR output contains nonfinite samples")
                block_start = output_cursor
                block_end = output_cursor + len(array)
                clip_start = max(block_start, start_frame)
                clip_end = min(block_end, end_frame)
                if clip_end > clip_start:
                    chosen = array[clip_start - block_start:clip_end - block_start]
                    main_handle.write(np.asarray(chosen[:, main_indices], dtype="<f4").tobytes(order="C"))
                    lfe_handle.write(np.asarray(chosen[:, lfe_indices], dtype="<f4").tobytes(order="C"))
                    selected_main += len(chosen)
                    selected_lfe += len(chosen)
                output_cursor = block_end

            for input_block in infile.iter_sample_blocks(BLOCK):
                if rendered_input >= end_frame:
                    break
                remaining = end_frame - rendered_input
                if len(input_block) > remaining:
                    input_block = input_block[:remaining]
                if not np.isfinite(input_block).all():
                    raise RuntimeError("ADM input contains nonfinite samples")
                emit(renderer.render(RATE, input_block))
                rendered_input += len(input_block)

            # EAR's object renderer has an internal decorrelator delay. Flush
            # it once, then require the requested interval to be complete.
            emit(renderer.get_tail(RATE, infile.channels))
            if rendered_input != end_frame:
                raise RuntimeError(f"input ended at {rendered_input}, expected {end_frame}")
            if selected_main != end_frame - start_frame or selected_lfe != selected_main:
                raise RuntimeError(
                    f"EAR output interval incomplete: main={selected_main}, lfe={selected_lfe}, "
                    f"expected={end_frame - start_frame}"
                )
    finally:
        main_handle.close()
        lfe_handle.close()

    manifest = {
        "renderer": "official EBU EAR ADM renderer 2.1.0",
        "adm": str(args.adm.resolve()),
        "admSha256": sha256(args.adm),
        "sampleRate": RATE,
        "inputChannels": input_channels,
        "layout": "9+10+3",
        "layoutChannels": [channel.name for channel in layout.channels],
        "mainChannelIndices": main_indices,
        "lfeChannelIndices": lfe_indices,
        "renderedInputFrames": rendered_input,
        "outputStartSec": args.start_sec,
        "outputEndSec": args.end_sec,
        "outputFrames": selected_main,
        "itemCounts": counts,
        "lfePolicy": (
            "preserve EAR LFE1/LFE2 as a separate 2ch sidecar; "
            f"{len(repairs['selectedDirectSpeakerLfeTracks'])} selected source DirectSpeakers LFE format(s) "
            "occupy the corresponding sidecar slot(s), and unused slots are silent; "
            "project BRIR receives the 22 non-LFE buses"
        ),
        "logicCompatibilityRepair": {
            **repairs,
            "earTimingFixesAppliedInMemory": True,
            "sourceUnmodified": True,
        },
        "outputs": {
            "systemH22chF32Raw": str(main_raw.resolve()),
            "lfe2chF32Raw": str(lfe_raw.resolve()),
        },
        "elapsedSec": time.monotonic() - start_time,
    }
    (args.out_dir / "ear-provenance.json").write_text(
        json.dumps(manifest, indent=2), encoding="utf-8"
    )
    print(json.dumps(manifest, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise
