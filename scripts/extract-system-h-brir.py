#!/usr/bin/env python3
import argparse
import hashlib
import os
import struct
import tempfile
from pathlib import Path

import h5py
import numpy as np

MAGIC = b"R2A1BRIR"
VERSION = 1
HEADER_SIZE = 520
E = 22
N = 16384
LABELS = [("M+060", 60, 0), ("M-060", -60, 0), ("M+000", 0, 0),
          ("M+135", 135, 0), ("M-135", -135, 0), ("M+030", 30, 0),
          ("M-030", -30, 0), ("M+180", 180, 0), ("M+090", 90, 0),
          ("M-090", -90, 0), ("U+045", 45, 30), ("U-045", -45, 30),
          ("U+000", 0, 30), ("T+000", 0, 90), ("U+135", 135, 30),
          ("U-135", -135, 30), ("U+090", 90, 30), ("U-090", -90, 30),
          ("U+180", 180, 30), ("B+000", 0, -30), ("B+045", 45, -30),
          ("B-045", -45, -30)]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def text(value):
    return value.decode("ascii") if isinstance(value, (bytes, np.bytes_)) else str(value)


def azclose(actual, expected):
    delta = abs(float(actual) - expected) % 360
    return min(delta, 360 - delta) <= 1e-6


def map_emitters(ep):
    require(ep.shape == (E, 3) and np.all(np.isfinite(ep)) and np.all(ep[:, 2] > 0),
            "emitter-invalid")
    mapping, errors = [], []
    for label, azimuth, elevation in LABELS:
        layer = label[0]
        hits = []
        for index, (az, el, _radius) in enumerate(ep):
            layer_ok = (abs(el) <= 1e-6 if layer == "M" else
                        abs(el - 90) <= 1e-6 if layer == "T" else
                        (el > 0 and el < 90) if layer == "U" else el < 0)
            if azclose(az, azimuth) and layer_ok:
                hits.append(index)
        require(len(hits) == 1, "emitter-mapping-" + label)
        mapping.append(hits[0])
        errors.append(abs(float(ep[hits[0], 1]) - elevation))
    require(len(set(mapping)) == E and max(errors) <= 10.0,
            "emitter-mapping-not-bijective-or-audit-limit")
    return mapping, max(errors)


def map_receivers(rp):
    require(rp.shape == (2, 3) and np.all(np.isfinite(rp)), "receiver-invalid")
    require(np.allclose(rp[:, [0, 2]], 0, atol=1e-9), "receiver-axis-invalid")
    require(rp[0, 1] * rp[1, 1] < 0 and
            abs(abs(rp[0, 1]) - abs(rp[1, 1])) <= 1e-9,
            "receiver-not-symmetric")
    return int(np.argmax(rp[:, 1])), int(np.argmin(rp[:, 1]))


def canonicalize_ir_slice(irslice, mapping, left, right):
    require(irslice.ndim == 3 and irslice.shape[0] == 2 and
            irslice.shape[1] == len(mapping), "ir-slice-shape")
    require(len(set(mapping)) == len(mapping), "ir-mapping-duplicate")
    return np.stack([[irslice[left, emitter, :] for emitter in mapping],
                     [irslice[right, emitter, :] for emitter in mapping]])


def canonical_delays(delay, mapping, left, right):
    require(delay.shape == (1, 2, E) and np.all(np.isfinite(delay)), "delay-invalid")
    return [[float(delay[0, left, i]) for i in mapping],
            [float(delay[0, right, i]) for i in mapping]]


def self_test():
    cases = 0
    ep = np.array([[az, el, 2.0] for _, az, el in LABELS], float)[::-1]
    mapping, _ = map_emitters(ep)
    require(mapping == list(range(E - 1, -1, -1)), "shuffle-map"); cases += 1
    duplicate = ep.copy(); duplicate[-1] = duplicate[-2]
    try: map_emitters(duplicate); raise AssertionError("duplicate-accepted")
    except ValueError: cases += 1
    missing = ep.copy(); missing[-1, 0] = 1
    try: map_emitters(missing); raise AssertionError("missing-accepted")
    except ValueError: cases += 1
    layer = ep.copy(); layer[0, 1] = 30
    try: map_emitters(layer); raise AssertionError("layer-accepted")
    except ValueError: cases += 1
    receivers = np.array([[0, .09, 0], [0, -.09, 0]], float)
    require(map_receivers(receivers) == (0, 1) and
            map_receivers(receivers[::-1]) == (1, 0), "receiver-map"); cases += 1
    for bad in (np.array([[0, .09, 0], [0, .08, 0]], float),
                np.array([[0, np.nan, 0], [0, -.09, 0]], float)):
        try: map_receivers(bad); raise AssertionError("receiver-invalid-accepted")
        except ValueError: cases += 1
    raw = np.empty((2, E, 4), float)
    for ear in range(2):
        for emitter in range(E):
            raw[ear, emitter] = [1000 * ear + emitter, 1000 * ear + emitter + 1,
                                 1000 * ear + emitter + 2, 1000 * ear + emitter + 3]
    canonical = canonicalize_ir_slice(raw, list(range(E - 1, -1, -1)), 1, 0)
    require(canonical[0, 0, 0] == 1021 and canonical[0, -1, -1] == 1003 and
            canonical[1, 0, 0] == 21 and canonical[1, -1, -1] == 3,
            "ir-canonical-markers"); cases += 1
    print(f"R2AExtractorSelfTest=PASS cases={cases} failures=0")


def extract(source, outpath):
    source, outpath = Path(source), Path(outpath)
    digest = hashlib.sha256()
    with source.open("rb") as source_file:
        for chunk in iter(lambda: source_file.read(1024 * 1024), b""):
            digest.update(chunk)
    with h5py.File(source, "r") as sofa:
        for key, expected in {"Conventions": "SOFA", "Version": "1.0",
                              "SOFAConventions": "MultiSpeakerBRIR",
                              "SOFAConventionsVersion": "0.3", "DataType": "FIRE"}.items():
            require(text(sofa.attrs.get(key, "")) == expected, "root-" + key)
        ir = sofa["Data.IR"]
        require(ir.shape == (180, 2, E, N) and ir.dtype.kind == "f" and ir.dtype.itemsize == 8,
                "ir-shape-or-dtype")
        sr = sofa["Data.SamplingRate"]
        require(sr.shape == (1,) and text(sr.attrs.get("Units", "")) == "hertz" and
                float(sr[0]) == 48000, "sampling-rate")
        delay = np.asarray(sofa["Data.Delay"][...])
        require(delay.shape == (1, 2, E) and np.all(np.isfinite(delay)), "delay")

        def coord(name, shape, kind, units):
            dataset = sofa[name]
            require(dataset.shape == shape and text(dataset.attrs.get("Type", "")) == kind and
                    text(dataset.attrs.get("Units", "")) == units, "coord-" + name)
            values = np.asarray(dataset[...])
            require(np.all(np.isfinite(values)), "finite-" + name)
            return values

        lp = coord("ListenerPosition", (1, 3), "cartesian", "metre")
        lv = coord("ListenerView", (180, 3), "cartesian", "metre")
        lu = np.asarray(sofa["ListenerUp"][...])
        require(lu.shape == (1, 3) and np.allclose(lp[0], [0, 0, 0]) and
                np.allclose(lu[0], [0, 0, 1]), "listener-frame")
        identity = np.flatnonzero(np.linalg.norm(lv - np.array([1., 0, 0]), axis=1) <= 1e-9)
        require(len(identity) == 1, "listener-view")
        m = int(identity[0])
        rp = coord("ReceiverPosition", (2, 3, 1), "cartesian", "metre")[:, :, 0]
        left, right = map_receivers(rp)
        ep = coord("EmitterPosition", (E, 3, 1), "spherical", "degree, degree, meter")[:, :, 0]
        mapping, max_error = map_emitters(ep)
        irslice = np.asarray(ir[m, ...])
        require(np.all(np.isfinite(irslice)), "identity-ir")
        canonical = canonicalize_ir_slice(irslice, mapping, left, right)
        delays = canonical_delays(delay, mapping, left, right)
        outpath.parent.mkdir(parents=True, exist_ok=True)
        fd, temporary = tempfile.mkstemp(prefix=outpath.name + ".", dir=outpath.parent)
        os.close(fd)
        try:
            with open(temporary, "wb") as output:
                header = (struct.pack("<8sIIIIIIiI", MAGIC, VERSION, HEADER_SIZE, 48000,
                                      E, 2, N, m, 32) + digest.digest() +
                          struct.pack("<22I", *mapping) + struct.pack("<2I", left, right) +
                          struct.pack("<44d", *(delays[0] + delays[1])))
                require(len(header) == HEADER_SIZE, "header-size")
                output.write(header)
                output.write(np.asarray(canonical, dtype="<f4").tobytes())
            os.replace(temporary, outpath)
        except Exception:
            try: os.unlink(temporary)
            except OSError: pass
            raise
    print(f"R2AExtract=PASS output={outpath} listenerM={m} leftReceiver={left} "
          f"rightReceiver={right} maxElevationAuditError={max_error:g} sha256={digest.hexdigest()}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("input", nargs="?"); parser.add_argument("output", nargs="?")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test: self_test()
    elif args.input and args.output: extract(args.input, args.output)
    else: parser.error("input and output required unless --self-test")
