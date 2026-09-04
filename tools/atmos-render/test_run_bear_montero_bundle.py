import importlib.util
import struct
import tempfile
import sys
import types
import unittest

import numpy as np


class _Metadata:
    def __init__(self):
        self.position = None
        self.gain = None
        self.width = None
        self.height = None
        self.depth = None


class _ObjectsInput:
    def __init__(self):
        self.type_metadata = _Metadata()
        self.rtime = None
        self.duration = None
        self.interpolationLength = None


class _Time:
    def __init__(self, numerator, denominator):
        self.numerator = numerator
        self.denominator = denominator


class _PolarPosition:
    def __init__(self, azimuth, elevation, distance):
        self.azimuth = azimuth
        self.elevation = elevation
        self.distance = distance


def _load_module():
    api = types.ModuleType("visr_bear.api")
    api.ObjectsInput = _ObjectsInput
    api.Time = _Time
    api.PolarPosition = _PolarPosition
    package = types.ModuleType("visr_bear")
    package.api = api
    sys.modules["visr_bear"] = package
    sys.modules["visr_bear.api"] = api
    spec = importlib.util.spec_from_file_location(
        "run_bear_montero_bundle_test_target",
        __file__.replace("test_run_bear_montero_bundle.py",
                         "run_bear_montero_bundle.py"),
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class BearMetadataAdapterTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.target = _load_module()

    def _state(self, **overrides):
        state = {
            "sourcePosition": 0,
            "rampDuration": 1536,
            "active": True,
            "gainMinusInfinity": False,
            "gainDb": 0.0,
            "standardX": 0.5,
            "standardY": 0.5,
            "standardZ": 0.0,
        }
        state.update(overrides)
        return state

    def test_legacy_state_without_schema_or_extent_is_accepted(self):
        oi = _ObjectsInput()
        ignored = self.target.apply_bear_metadata(oi, self._state(), 0, 1536)
        self.assertEqual(oi.type_metadata.width, None)
        self.assertEqual(oi.type_metadata.height, None)
        self.assertEqual(oi.type_metadata.depth, None)
        self.assertNotIn("divergence", ignored)

    def test_extent_requires_explicit_presence(self):
        absent = _ObjectsInput()
        self.target.apply_bear_metadata(
            absent, self._state(extent={"width": 9, "height": 9, "depth": 9}), 0, 1536
        )
        self.assertIsNone(absent.type_metadata.width)

        explicit_zero = _ObjectsInput()
        self.target.apply_bear_metadata(
            explicit_zero,
            self._state(
                extentPresence="explicit-zero",
                extent={"width": 0, "height": 0, "depth": 0},
            ),
            0,
            1536,
        )
        self.assertEqual(
            (explicit_zero.type_metadata.width, explicit_zero.type_metadata.height,
             explicit_zero.type_metadata.depth),
            (0.0, 0.0, 0.0),
        )

        non_zero = _ObjectsInput()
        self.target.apply_bear_metadata(
            non_zero,
            self._state(
                extentPresence="non-zero",
                extent={"width": 0.2, "height": 0.3, "depth": 0.4},
            ),
            0,
            1536,
        )
        self.assertEqual(
            (non_zero.type_metadata.width, non_zero.type_metadata.height,
             non_zero.type_metadata.depth),
            (0.2, 0.3, 0.4),
        )

    def test_divergence_never_maps_to_diffuse_and_reserved_warp_is_audited(self):
        oi = _ObjectsInput()
        ignored = self.target.apply_bear_metadata(
            oi,
            self._state(
                divergence={
                    "present": True, "reused": False, "mode": 0,
                    "index": 1, "value": 0.5,
                },
                warpMode="reserved",
                screenAnchored=True,
                distanceInfinite=True,
            ),
            0,
            1536,
        )
        self.assertIsNone(getattr(oi.type_metadata, "diffuse", None))
        self.assertEqual(ignored["divergence"], 1)
        self.assertEqual(ignored["reservedWarp"], 1)
        self.assertEqual(ignored["screenAnchored"], 1)
        self.assertEqual(ignored["infiniteDistance"], 1)

    def test_bs2127_ear_position_conversion_and_fail_closed_boundaries(self):
        cases = [
            ((0.0, 0.0, 0.0), (30.0, 0.0, 1.0)),
            ((1.0, 0.0, 0.0), (-30.0, 0.0, 1.0)),
            ((0.0, 0.5, 0.0), (70.0, 0.0, 1.0)),
            ((1.0, 0.5, 0.0), (-70.0, 0.0, 1.0)),
            ((0.0, 1.0, 0.0), (110.0, 0.0, 1.0)),
            ((1.0, 1.0, 0.0), (-110.0, 0.0, 1.0)),
            ((0.5, 0.5, 0.0), (0.0, 0.0, 0.0)),
            ((0.5, 0.5, 1.0), (0.0, 90.0, 1.0)),
        ]
        for standard_x, standard_y, standard_z, expected in (
            (*position, result) for position, result in cases
        ):
            actual = self.target.polar(self._state(
                standardX=standard_x, standardY=standard_y, standardZ=standard_z))
            self.assertEqual(actual, expected)
        with self.assertRaises(ValueError):
            self.target.polar(self._state(standardX=float("nan")))
        self.assertEqual(
            self.target.polar(self._state(standardZ=2.0)), (0.0, 90.0, 2.0)
        )

    def test_stream_writer_and_latency_sink_preserve_first_and_tail_samples(self):
        with tempfile.TemporaryDirectory() as directory:
            path = f"{directory}/aligned.wav"
            writer = self.target._StreamWavWriter(path, "f32")
            sink = self.target._LatencyCompensatedSink((writer,), 2, 4)
            sink.write(np.array([[0.0, 0.0], [0.0, 0.0],
                                 [1.0, 10.0], [2.0, 20.0]], np.float32))
            # The second block is the renderer's flushed tail.  It must be
            # available after the two delayed samples and must be clipped to
            # exactly the input frame count.
            sink.write(np.array([[3.0, 30.0], [4.0, 40.0],
                                 [99.0, 99.0]], np.float32))
            self.assertEqual(sink.frames, 4)
            report = writer.close()
            self.assertEqual(report["frames"], 4)
            with open(path, "rb") as stream:
                payload = stream.read()[44:]
            actual = np.frombuffer(payload, dtype="<f4").reshape(-1, 2)
            np.testing.assert_array_equal(
                actual, np.array([[1, 10], [2, 20], [3, 30], [4, 40]], np.float32))
            self.assertEqual(len(payload), 4 * 2 * 4)

    def test_audio_blocks_keep_global_phase_across_batch_boundaries(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = __import__("pathlib").Path(directory)
            for number, (start, count) in enumerate(((0, 500), (500, 20))):
                audio = np.zeros((15, count), dtype="<f4")
                audio[0] = np.arange(start, start + count, dtype=np.float32)
                payload = (b"BSCN" + struct.pack("<IIIqq", 2, 15, count,
                                                    start, start + count) +
                           audio.tobytes() + struct.pack("<I", 0))
                (directory / f"batch-{number:08d}.bin").write_bytes(payload)
            blocks = list(self.target._iter_audio_blocks([
                {"path": directory / "batch-00000000.bin", "start": 0,
                 "end": 500, "samples": 500},
                {"path": directory / "batch-00000001.bin", "start": 500,
                 "end": 520, "samples": 20},
            ]))
            self.assertEqual([(block.shape[1], n, start)
                              for block, n, start in blocks],
                             [(512, 512, 0), (512, 8, 512)])
            np.testing.assert_array_equal(blocks[0][0][0], np.arange(512, dtype=np.float32))
            np.testing.assert_array_equal(blocks[1][0][0, :8], np.arange(512, 520, dtype=np.float32))


if __name__ == "__main__":
    unittest.main()
