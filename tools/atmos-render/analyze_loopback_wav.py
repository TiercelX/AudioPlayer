"""Conservative first-pass analyzer for a WASAPI loopback WAV sidecar.

It reports signal statistics and threshold censoring.  A program recording is
not treated as an impulse response or a proof of pleasant playback.
"""

from __future__ import annotations

import argparse
import json
import math
import struct
import tempfile
import unittest
from pathlib import Path

import numpy as np


BANDS = ((0.0, 80.0), (80.0, 200.0), (200.0, 1000.0),
         (1000.0, 5000.0), (5000.0, 20000.0))


# GUID fields are stored in RIFF's little-endian GUID byte layout.  The
# canonical IEEE_FLOAT GUID {00000003-0000-0010-8000-00AA00389B71} therefore
# starts 03 00 00 00 00 00 10 00 (not ...00 10 00).
_PCM_GUID = bytes.fromhex("0100000000001000800000AA00389B71")
_FLOAT_GUID = bytes.fromhex("0300000000001000800000AA00389B71")


def _riff_chunks(raw: bytes):
    if len(raw) < 12 or raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")
    offset = 12
    while offset + 8 <= len(raw):
        kind = raw[offset:offset + 4]
        size = struct.unpack_from("<I", raw, offset + 4)[0]
        start = offset + 8
        end = start + size
        if end > len(raw):
            raise ValueError("truncated RIFF chunk")
        yield kind, raw[start:end]
        offset = end + (size & 1)


def read_wav(path: Path) -> tuple[np.ndarray, int, str]:
    """Read the small, documented WAV subset emitted by loopback capture.

    This deliberately does not use ``wave``: Windows capture commonly emits
    WAVE_FORMAT_EXTENSIBLE, which is valid RIFF but is rejected by some Python
    versions.  The format tag is used to distinguish PCM32 from float32.
    """
    raw = path.read_bytes()
    fmt = data = None
    for kind, chunk in _riff_chunks(raw):
        if kind == b"fmt " and fmt is None:
            fmt = chunk
        elif kind == b"data" and data is None:
            data = chunk
    if fmt is None or data is None or len(fmt) < 16:
        raise ValueError("WAV must contain fmt and data chunks")
    tag, channels, rate, byte_rate, block_align, bits = struct.unpack_from("<HHIIHH", fmt, 0)
    if channels <= 0 or rate <= 0 or block_align <= 0 or byte_rate <= 0:
        raise ValueError("invalid WAV format fields")
    encoding_tag = tag
    if tag == 0xFFFE:
        if len(fmt) < 40:
            raise ValueError("truncated WAVE_FORMAT_EXTENSIBLE fmt chunk")
        cb_size = struct.unpack_from("<H", fmt, 16)[0]
        if cb_size < 22:
            raise ValueError("invalid extensible fmt size")
        subformat = fmt[24:40]
        if subformat == _PCM_GUID:
            encoding_tag = 1
        elif subformat == _FLOAT_GUID:
            encoding_tag = 3
        else:
            raise ValueError("unsupported WAVE_FORMAT_EXTENSIBLE subformat")
    if encoding_tag not in (1, 3):
        raise ValueError(f"unsupported WAV format tag: 0x{tag:04x}")
    bytes_per_sample = (bits + 7) // 8
    if bits not in ((8, 16, 24, 32) if encoding_tag == 1 else (32, 64)):
        raise ValueError(f"unsupported {('PCM' if encoding_tag == 1 else 'float')} bit depth: {bits}")
    if block_align != channels * bytes_per_sample or len(data) % block_align:
        raise ValueError("invalid WAV block alignment or truncated payload")
    frames = len(data) // block_align
    if encoding_tag == 3 and bits == 32:
        values = np.frombuffer(data, dtype="<f4")
        encoding = "float32" + ("-extensible" if tag == 0xFFFE else "")
    elif encoding_tag == 3:
        values = np.frombuffer(data, dtype="<f8")
        encoding = "float64" + ("-extensible" if tag == 0xFFFE else "")
    elif bits == 8:
        values = (np.frombuffer(data, dtype=np.uint8).astype(np.float32) - 128.0) / 128.0
        encoding = "pcm8"
    elif bits == 16:
        values = np.frombuffer(data, dtype="<i2").astype(np.float32) / 32768.0
        encoding = "pcm16"
    elif bits == 24:
        packed = np.frombuffer(data, dtype=np.uint8).reshape(-1, 3)
        signed = (packed[:, 0].astype(np.int32) |
                  (packed[:, 1].astype(np.int32) << 8) |
                  (packed[:, 2].astype(np.int32) << 16))
        signed = (signed ^ 0x800000) - 0x800000
        values = signed.astype(np.float32) / 8388608.0
        encoding = "pcm24"
    else:
        values = np.frombuffer(data, dtype="<i4").astype(np.float32) / 2147483648.0
        encoding = "pcm32"
    if values.size != frames * channels:
        raise ValueError("truncated WAV payload")
    values = values.reshape(frames, channels)
    if not np.isfinite(values).all():
        raise ValueError("non-finite WAV samples")
    return values, rate, encoding


def _channel_stats(samples: np.ndarray, rate: int) -> dict:
    peak = float(np.max(np.abs(samples))) if len(samples) else 0.0
    rms = float(np.sqrt(np.mean(samples.astype(np.float64) ** 2))) if len(samples) else 0.0
    threshold = max(1.0e-6, peak * 1.0e-3)
    valid = np.flatnonzero(np.abs(samples) >= threshold)
    onset = int(valid[0]) if len(valid) else None
    bands = np.zeros(len(BANDS), dtype=np.float64)
    block = 4096
    for start in range(0, len(samples), block):
        chunk = samples[start:start + block]
        if len(chunk) < 16:
            continue
        window = np.hanning(len(chunk))
        power = np.abs(np.fft.rfft(chunk * window)) ** 2
        frequencies = np.fft.rfftfreq(len(chunk), 1.0 / rate)
        for index, (low, high) in enumerate(BANDS):
            bands[index] += float(power[(frequencies >= low) & (frequencies < high)].sum())
    total = float(bands.sum())
    decay = []
    for db in (20, 40, 60):
        limit = peak * 10.0 ** (-db / 20.0)
        above = np.flatnonzero(np.abs(samples[(onset or 0):]) >= limit)
        if onset is None or not len(above):
            decay.append({"db": db, "milliseconds": None, "censored": True, "reason": "no-valid-transient"})
        else:
            last = int(above[-1]) + (onset or 0)
            decay.append({"db": db, "milliseconds": 1000.0 * (last - (onset or 0)) / rate,
                          "censored": last >= len(samples) - 1,
                          "reason": "program-envelope-or-tail-censored" if last >= len(samples) - 1 else "threshold-crossing"})
    return {
        "frames": int(len(samples)), "durationSeconds": len(samples) / rate,
        "peak": peak, "rms": rms, "crestFactor": peak / rms if rms else None,
        "clipSamplesAbsGt1": int(np.count_nonzero(np.abs(samples) > 1.0)),
        "firstValidTransientSample": onset,
        "firstValidTransientMilliseconds": 1000.0 * onset / rate if onset is not None else None,
        "noiseFloorOrCensoring": "no-valid-transient" if onset is None else "threshold-decay-is-program-envelope-not-reverb-proof",
        "decay": decay,
        "bandEnergy": [{"lowHz": low, "highHz": high, "energy": float(bands[index]),
                        "fraction": float(bands[index] / total) if total else 0.0}
                       for index, (low, high) in enumerate(BANDS)],
        "frequencyMethod": "4096-frame Hann-windowed real FFT blocks; not a calibrated filterbank",
    }


def analyze(path: Path) -> dict:
    audio, rate, encoding = read_wav(path)
    channels = [_channel_stats(audio[:, index], rate) for index in range(audio.shape[1])]
    # A frame downmix preserves the sample rate and timebase.  Flattening an
    # interleaved buffer would double the apparent duration and corrupt FFT Hz.
    combined = _channel_stats(np.mean(audio, axis=1, dtype=np.float64).astype(np.float32), rate)
    return {
        "schema": "audioplayer.loopback-wav-analysis.v1",
        "result": "PASS" if any(item["firstValidTransientSample"] is not None for item in channels) else "INCONCLUSIVE_NO_VALID_TRANSIENT",
        "evidenceLayer": "loopback-wav-signal-statistics",
        "wav": str(path.resolve()), "sampleRate": rate, "channels": audio.shape[1], "encoding": encoding,
        "channelStats": channels, "combinedFrameDownmixStats": combined,
        "limitations": ["No-valid-transient is INCONCLUSIVE, not a quality verdict.",
                        "Program material is not an impulse response; decay values are censored/envelope evidence only.",
                        "No loudness normalization or subjective listening claim is made."],
    }


class LoopbackAnalyzerTests(unittest.TestCase):
    @staticmethod
    def _write_wav(path: Path, samples: np.ndarray, rate: int, tag: int, bits: int,
                   extensible: bool = False) -> None:
        samples = np.asarray(samples)
        channels = samples.shape[1] if samples.ndim == 2 else 1
        if samples.ndim == 1:
            samples = samples[:, None]
        if tag == 3:
            payload = samples.astype("<f4" if bits == 32 else "<f8").tobytes()
        elif bits == 16:
            payload = np.clip(samples * 32768.0, -32768, 32767).astype("<i2").tobytes()
        elif bits == 32:
            payload = np.clip(samples * 2147483648.0, -2147483648, 2147483647).astype("<i4").tobytes()
        else:
            raise AssertionError("test helper only needs PCM16/32 and float32")
        actual_tag = 0xFFFE if extensible else tag
        bytes_per_sample = bits // 8
        block_align = channels * bytes_per_sample
        fmt = struct.pack("<HHIIHH", actual_tag, channels, rate,
                          rate * block_align, block_align, bits)
        if extensible:
            subformat = _FLOAT_GUID if tag == 3 else _PCM_GUID
            fmt += struct.pack("<H H I", 22, bits, 0) + subformat
        def chunk(kind: bytes, body: bytes) -> bytes:
            return kind + struct.pack("<I", len(body)) + body + (b"\0" if len(body) & 1 else b"")
        body = chunk(b"fmt ", fmt) + chunk(b"data", payload)
        path.write_bytes(b"RIFF" + struct.pack("<I", 4 + len(body)) + b"WAVE" + body)

    def test_impulse_is_detected_and_decay_is_censored(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "impulse.wav"
            samples = np.zeros((4800, 2), dtype=np.float32)
            samples[100, :] = 0.5
            self._write_wav(path, samples, 48000, 3, 32)
            report = analyze(path)
            self.assertEqual(report["result"], "PASS")
            self.assertEqual(report["channelStats"][0]["firstValidTransientSample"], 100)
            self.assertFalse(report["channelStats"][0]["decay"][0]["censored"])

    def test_silence_is_inconclusive(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "silence.wav"
            self._write_wav(path, np.zeros(4800), 48000, 1, 16)
            self.assertEqual(analyze(path)["result"], "INCONCLUSIVE_NO_VALID_TRANSIENT")

    def test_pcm32_is_not_decoded_as_float(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "pcm32.wav"
            self._write_wav(path, np.array([0.25, 0.0, -0.25]), 48000, 1, 32)
            audio, rate, encoding = read_wav(path)
            self.assertEqual((rate, encoding), (48000, "pcm32"))
            np.testing.assert_allclose(audio[:, 0], [0.25, 0.0, -0.25], atol=2e-9)

    def test_extensible_float32_is_decoded(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "extensible-float.wav"
            self._write_wav(path, np.array([[0.5], [0.0]]), 48000, 3, 32, extensible=True)
            audio, _, encoding = read_wav(path)
            self.assertEqual(encoding, "float32-extensible")
            self.assertEqual(float(audio[0, 0]), 0.5)

    def test_combined_stats_are_frame_downmix(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "stereo.wav"
            self._write_wav(path, np.column_stack((np.ones(4800) * .5, np.zeros(4800))), 48000, 3, 32)
            report = analyze(path)
            self.assertIn("combinedFrameDownmixStats", report)
            self.assertNotIn("combinedInterleavedStats", report)
            self.assertEqual(report["combinedFrameDownmixStats"]["frames"], 4800)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("wav", type=Path, nargs="?")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        result = unittest.TextTestRunner(verbosity=1).run(unittest.defaultTestLoader.loadTestsFromTestCase(LoopbackAnalyzerTests))
        raise SystemExit(0 if result.wasSuccessful() else 1)
    if args.wav is None or args.output is None:
        parser.error("wav and --output are required unless --self-test is used")
    report = analyze(args.wav)
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"result": report["result"], "report": str(args.output.resolve())}))


if __name__ == "__main__":
    main()
