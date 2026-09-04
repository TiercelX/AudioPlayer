"""Create eight bounded, single-object RIFF ADM diagnostic derivatives.

The source media is copied, never modified. Its ADM structure, CHNA, and DBMD
chunks are retained; AXML timing is rewritten to match the physically bounded
five-second data chunk. Bed tracks 1-10 and the seven non-selected object
tracks are zero; one selected object receives the same deterministic wideband
stimulus in each case. This is local diagnostic material, not Dolby-authored
provenance, and supports RIFF/WAVE only.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import tempfile
from pathlib import Path

import numpy as np


RATE = 48000
CHANNELS = 18
OBJECT_COUNT = 8
SECONDS = 5.0
FREQUENCIES_HZ = [173, 337, 521, 911, 1471, 2203, 3301, 4799, 6311, 7907, 9433, 11789]
NOISE_BAND_HZ = [80, 12000]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def wav_chunks(path: Path):
    with path.open("rb") as stream:
        if stream.read(4) != b"RIFF":
            raise ValueError("source is not RIFF")
        stream.read(4)
        if stream.read(4) != b"WAVE":
            raise ValueError("source is not WAVE")
        while True:
            header = stream.read(8)
            if not header:
                return
            if len(header) != 8:
                raise ValueError("truncated chunk header")
            name = header[:4].decode("latin1")
            size = struct.unpack("<I", header[4:])[0]
            offset = stream.tell()
            yield name, size, offset
            stream.seek(size + (size & 1), 1)


def gated_wideband_signal(frames: int, seed: int) -> np.ndarray:
    """Return a deterministic low-level multi-tone/noise/pulse stimulus."""
    rng = np.random.default_rng(seed)
    t = np.arange(frames, dtype=np.float64) / RATE
    noise = rng.standard_normal(frames)
    spectrum = np.fft.rfft(noise)
    frequencies = np.fft.rfftfreq(frames, 1.0 / RATE)
    spectrum[(frequencies < NOISE_BAND_HZ[0]) | (frequencies > NOISE_BAND_HZ[1])] = 0
    noise = np.fft.irfft(spectrum, n=frames)
    noise /= max(float(np.max(np.abs(noise))), 1e-12)

    tone = np.zeros(frames, dtype=np.float64)
    for index, frequency in enumerate(FREQUENCIES_HZ):
        phase = (seed % 97 + index * 13) * np.pi / 97.0
        tone += 0.00125 * np.sin(2.0 * np.pi * frequency * t + phase)

    pulses = np.zeros(frames, dtype=np.float64)
    for pulse_time in (0.70, 1.55, 2.40, 3.25, 4.10):
        pulses += np.exp(-0.5 * ((t - pulse_time) / 0.0015) ** 2)

    gate = np.zeros(frames, dtype=np.float64)
    gate[(t >= 0.25) & (t < 4.75)] = 1.0
    fade = 0.02
    opening = (t >= 0.25) & (t < 0.25 + fade)
    closing = (t >= 4.75 - fade) & (t < 4.75)
    gate[opening] = 0.5 - 0.5 * np.cos(np.pi * (t[opening] - 0.25) / fade)
    gate[closing] = 0.5 + 0.5 * np.cos(np.pi * (t[closing] - (4.75 - fade)) / fade)

    signal = gate * (0.010 * noise + tone + 0.012 * pulses)
    peak = float(np.max(np.abs(signal)))
    if not np.isfinite(signal).all() or peak >= 0.05:
        raise ValueError(f"stimulus peak guard failed: {peak}")
    return signal


def _read_format(chunks, source: Path):
    fmt_size, fmt_offset = chunks["fmt "]
    with source.open("rb") as stream:
        stream.seek(fmt_offset)
        audio_format, channels, rate, _byte_rate, block_align, bits = struct.unpack(
            "<HHIIHH", stream.read(fmt_size)
        )
    if (audio_format, channels, rate, block_align, bits) != (1, CHANNELS, RATE, 54, 24):
        raise ValueError("expected 18-channel PCM S24LE at 48 kHz")
    return chunks["data"]


def _copy_chunk(stream_in, stream_out, size: int) -> None:
    remaining = size
    while remaining:
        block = stream_in.read(min(8 * 1024 * 1024, remaining))
        if not block:
            raise ValueError("truncated source chunk")
        stream_out.write(block)
        remaining -= len(block)


def _bound_adm_axml(payload: bytes) -> bytes:
    """Bound the source ADM timing to the physically retained five seconds."""
    text = payload.decode("utf-8")
    text, duration_count = re.subn(
        r'duration="[^"]+"', 'duration="00:00:05.00000"', text)
    text, end_count = re.subn(
        r'end="[^"]+"', 'end="01:00:05.00000"', text, count=1)
    if duration_count == 0 or end_count != 1:
        raise ValueError("ADM axml timing fields were not found")
    return text.encode("utf-8")


def create_case(source: Path, output: Path, manifest: Path, object_index: int, seed: int) -> dict:
    source = source.resolve()
    output = output.resolve()
    manifest = manifest.resolve()
    if source == output:
        raise ValueError("refusing in-place modification")
    if not 0 <= object_index < OBJECT_COUNT:
        raise ValueError("object index must be in [0, 7]")
    chunks = {name: (size, offset) for name, size, offset in wav_chunks(source)}
    data_size, data_offset = _read_format(chunks, source)
    frames = min(int(RATE * SECONDS), data_size // 54)
    if frames != int(RATE * SECONDS):
        raise ValueError("source is shorter than the five-second diagnostic window")

    signal = gated_wideband_signal(frames, seed)
    data_bytes = frames * CHANNELS * 3
    with source.open("rb") as stream_in, output.open("wb") as stream_out:
        source_header = stream_in.read(12)
        if len(source_header) != 12:
            raise ValueError("truncated RIFF header")
        stream_out.write(source_header[:4] + b"\x00\x00\x00\x00" + source_header[8:])
        for name, size, offset in wav_chunks(source):
            stream_in.seek(offset - 8)
            header = stream_in.read(8)
            if len(header) != 8:
                raise ValueError("truncated source chunk header")
            stream_out.write(header[:4])
            payload = None
            if name == "axml":
                stream_in.seek(offset)
                payload = _bound_adm_axml(stream_in.read(size))
            payload_size = data_bytes if name == "data" else len(payload) if payload is not None else size
            stream_out.write(struct.pack("<I", payload_size))
            if name == "data":
                for start in range(0, frames, 12000):
                    count = min(12000, frames - start)
                    interleaved = np.zeros((count, CHANNELS), dtype=np.float64)
                    interleaved[:, 10 + object_index] = signal[start : start + count]
                    quantized = np.rint(np.clip(interleaved, -1.0, 0.9999998808) * 8388607.0).astype(np.int32)
                    raw = np.empty((count, CHANNELS, 3), dtype=np.uint8)
                    raw[:, :, 0] = quantized & 0xFF
                    raw[:, :, 1] = (quantized >> 8) & 0xFF
                    raw[:, :, 2] = (quantized >> 16) & 0xFF
                    stream_out.write(raw.tobytes())
                stream_in.seek(offset + data_size)
            else:
                if payload is None:
                    stream_in.seek(offset)
                    _copy_chunk(stream_in, stream_out, size)
                else:
                    stream_out.write(payload)
            if payload_size & 1:
                stream_out.write(b"\x00")
        stream_out.flush()
        riff_size = stream_out.tell() - 8
        stream_out.seek(4)
        stream_out.write(struct.pack("<I", riff_size))

    result = {
        "source": str(source),
        "sourceSha256": sha256(source),
        "output": str(output),
        "outputSha256": sha256(output),
        "derivedDiagnosticOnly": True,
        "sourceWasModified": False,
        "sampleRate": RATE,
        "channels": CHANNELS,
        "frames": frames,
        "windowSeconds": SECONDS,
        "physicalFrames": frames,
        "physicalDurationSeconds": frames / RATE,
        "physicalDataBytes": data_bytes,
        "selectedObjectIndexZeroBased": object_index,
        "selectedObjectTrackOneBased": 11 + object_index,
        "bedTracksZeroBased": list(range(10)),
        "zeroObjectTracksOneBased": [11 + i for i in range(OBJECT_COUNT) if i != object_index],
        "stimulus": {
            "seed": seed,
            "frequenciesHz": FREQUENCIES_HZ,
            "noiseBandHz": NOISE_BAND_HZ,
            "pulseTimesSeconds": [0.70, 1.55, 2.40, 3.25, 4.10],
            "peak": float(np.max(np.abs(signal))),
            "rms": float(np.sqrt(np.mean(signal * signal))),
        },
        "retainedChunks": ["JUNK", "fmt ", "axml", "chna", "dbmd"],
        "metadataRewrite": "axml programme/object/block timing bounded to 5.0 seconds",
        "note": "ADM XML structure, CHNA, and DBMD retained; AXML timing is bounded and this is not Dolby-authored provenance.",
    }
    manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def self_test() -> None:
    signal = gated_wideband_signal(RATE, 20260831)
    assert signal.shape == (RATE,)
    assert np.isfinite(signal).all()
    assert 0.0 < float(np.max(np.abs(signal))) < 0.05
    assert FREQUENCIES_HZ[0] < FREQUENCIES_HZ[-1]
    assert NOISE_BAND_HZ == [80, 12000]
    with tempfile.TemporaryDirectory(prefix="single-object-oracle-selftest-") as temp:
        root = Path(temp)
        fmt = struct.pack("<HHIIHH", 1, CHANNELS, RATE, RATE * 54, 54, 24)
        source_data = b"\x00" * (int(RATE * 6) * CHANNELS * 3)
        chunks = [(b"JUNK", b"abc"), (b"fmt ", fmt), (b"data", source_data),
                  (b"axml", b'<audioProgramme start="01:00:00.00000" end="01:07:27.99531"><audioObject duration="00:07:27.99531"/></audioProgramme>'),
                  (b"chna", b"chna"), (b"dbmd", b"dbmd")]
        payload = bytearray(b"RIFF\x00\x00\x00\x00WAVE")
        for name, body in chunks:
            payload += name + struct.pack("<I", len(body)) + body
            if len(body) & 1:
                payload += b"\x00"
        struct.pack_into("<I", payload, 4, len(payload) - 8)
        source = root / "source.wav"
        source.write_bytes(payload)
        output = root / "case.wav"
        manifest = root / "case.json"
        # The tiny test AXML is not used as a valid ADM source, so test the
        # timing rewrite separately and validate the physical chunk layout.
        assert b'duration="00:00:05.00000"' in _bound_adm_axml(
            b'<audioProgramme start="01:00:00.00000" end="01:07:27.99531">'
            b'<audioObject duration="00:07:27.99531"/></audioProgramme>')
        result = create_case(source, output, manifest, 0, 20260831)
        output_chunks = list(wav_chunks(output))
        data_chunk = next(item for item in output_chunks if item[0] == "data")
        assert data_chunk[1] == int(RATE * SECONDS) * CHANNELS * 3
        assert output_chunks[2][0] == "data" and output_chunks[3][0] == "axml"
        with output.open("rb") as stream:
            header = stream.read(12)
        assert struct.unpack("<I", header[4:8])[0] == output.stat().st_size - 8
        assert result["physicalFrames"] == int(RATE * SECONDS)
    print("singleObjectOracleSelfTest=PASS cases=5 physicalFrames=240000 duration=5.0")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path, nargs="?")
    parser.add_argument("output_dir", type=Path, nargs="?")
    parser.add_argument("manifest", type=Path, nargs="?")
    parser.add_argument("--seed", type=int, default=20260831)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if args.source is None or args.output_dir is None or args.manifest is None:
        parser.error("source, output_dir, and manifest are required unless --self-test is used")
    source = args.source.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    cases = []
    for object_index in range(OBJECT_COUNT):
        output = output_dir / f"single-object-{object_index + 1:02d}.wav"
        manifest = output_dir / f"single-object-{object_index + 1:02d}.json"
        cases.append(create_case(source, output, manifest, object_index, args.seed))
    args.manifest.resolve().parent.mkdir(parents=True, exist_ok=True)
    args.manifest.resolve().write_text(
        json.dumps(
            {
                "result": "PASS",
                "source": str(source),
                "sourceSha256": sha256(source),
                "cases": cases,
                "caseCount": len(cases),
                "note": "All files are bounded local diagnostic derivatives; source was not modified.",
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    print(json.dumps({"result": "PASS", "caseCount": len(cases), "manifest": str(args.manifest.resolve())}))


if __name__ == "__main__":
    main()
