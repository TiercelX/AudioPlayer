"""Render a Gate6C BEAR scene bundle without touching the player runtime."""
import argparse, hashlib, json, math, os, struct, subprocess, sys
from pathlib import Path
import numpy as np

RATE = 48000
LATENCY = 167
WINDOWS = ((50*RATE,60*RATE,"object-movement"),(75*RATE,90*RATE,"stable-vocal"),(114*RATE,125*RATE,"dense-complex"))

def time_arg(n, d=RATE):
    from visr_bear.api import Time
    return Time(int(n), int(d))

class _StreamWavWriter:
    """Small seek-back WAV writer used by full-file mode.

    BEAR can render hours of input, so full-file mode must not retain the
    stereo result in a Python list.  The header is patched on close after the
    frame count and statistics are known.
    """

    def __init__(self, path, kind, gain_db=0.0):
        if kind not in ("f32", "s24"):
            raise ValueError(f"unsupported WAV kind: {kind}")
        self.path = Path(path)
        self.kind = kind
        self.gain = 10.0 ** (float(gain_db) / 20.0)
        self.file = self.path.open("wb+")
        self.file.write(b"\0" * 44)
        self.frames = 0
        self.peak = 0.0
        self.sum_squares = 0.0
        self.any_nonzero = False
        self.clipping = False
        self.closed = False

    def write(self, samples):
        data = np.asarray(samples, dtype=np.float32)
        if data.ndim != 2 or data.shape[1] != 2 or not np.isfinite(data).all():
            raise RuntimeError("invalid output shape/nonfinite")
        if not len(data):
            return
        data = data * self.gain
        abs_data = np.abs(data)
        self.peak = max(self.peak, float(np.max(abs_data)))
        self.sum_squares += float(np.sum(data.astype(np.float64) ** 2))
        self.any_nonzero = self.any_nonzero or bool(np.any(data))
        self.clipping = self.clipping or bool(np.any(abs_data > 1.0))
        if self.kind == "f32":
            self.file.write(data.astype("<f4", copy=False).tobytes())
        else:
            pcm = np.clip(data, -1.0, 0.9999999)
            q = np.rint(pcm * 8388607.0).astype(np.int32)
            raw = np.empty((q.shape[0], 6), dtype=np.uint8)
            raw[:, 0] = q[:, 0] & 255
            raw[:, 1] = (q[:, 0] >> 8) & 255
            raw[:, 2] = (q[:, 0] >> 16) & 255
            raw[:, 3] = q[:, 1] & 255
            raw[:, 4] = (q[:, 1] >> 8) & 255
            raw[:, 5] = (q[:, 1] >> 16) & 255
            self.file.write(raw.tobytes())
        self.frames += int(data.shape[0])

    def close(self):
        if self.closed:
            return self._report
        if self.frames <= 0 or not self.any_nonzero:
            self.file.close()
            self.closed = True
            raise RuntimeError("silent or empty BEAR output")
        bytes_per_frame = 8 if self.kind == "f32" else 6
        payload_size = self.frames * bytes_per_frame
        if payload_size > 0xFFFFFFFF - 36:
            self.file.close()
            self.closed = True
            raise RuntimeError("RIFF WAV exceeds 4 GiB")
        audio_format, bits = ((3, 32) if self.kind == "f32" else (1, 24))
        fmt = struct.pack("<HHIIHH", audio_format, 2, RATE,
                          RATE * bytes_per_frame, bytes_per_frame, bits)
        header = (b"RIFF" + struct.pack("<I", 36 + payload_size) + b"WAVE"
                  + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data"
                  + struct.pack("<I", payload_size))
        self.file.seek(0)
        self.file.write(header)
        self.file.flush()
        self.file.close()
        self.closed = True
        self._report = {
            "frames": self.frames,
            "duration": self.frames / RATE,
            "peak": self.peak,
            "rms": math.sqrt(self.sum_squares / (self.frames * 2)),
            "finite": True,
            "clipping": self.clipping,
        }
        return self._report


def write_f32(path, samples):
    writer = _StreamWavWriter(path, "f32")
    writer.write(samples)
    return writer.close()


def write_wav(path, samples, gain_db=0.0):
    writer = _StreamWavWriter(path, "s24", gain_db)
    writer.write(samples)
    return writer.close()


class _LatencyCompensatedSink:
    """Discard renderer latency, then write exactly target input frames."""

    def __init__(self, sinks, latency, target_frames):
        self.sinks = tuple(sinks)
        self.skip = int(latency)
        self.target_frames = int(target_frames)
        self.frames = 0

    def write(self, samples):
        data = np.asarray(samples, dtype=np.float32)
        if data.ndim != 2 or data.shape[1] != 2:
            raise RuntimeError("invalid latency sink shape")
        if self.skip:
            skipped = min(self.skip, len(data))
            self.skip -= skipped
            data = data[skipped:]
        remaining = self.target_frames - self.frames
        if remaining <= 0 or not len(data):
            return
        data = data[:remaining]
        for sink in self.sinks:
            sink.write(data)
        self.frames += len(data)

_EAR_BS2127_MAPPING = (
    (0.0, (0.0, 1.0)), (-30.0, (1.0, 1.0)), (-110.0, (1.0, -1.0)),
    (110.0, (-1.0, -1.0)), (30.0, (-1.0, 1.0)),
)


def _relative_angle(start, end):
    while end - 360.0 >= start:
        end -= 360.0
    while end < start:
        end += 360.0
    return end


def _inside_angle_range(value, start, end):
    while end - 360.0 > start:
        end -= 360.0
    while end < start:
        end += 360.0
    while value - 360.0 >= start:
        value -= 360.0
    while value < start:
        value += 360.0
    return value <= end


def _cartesian_azimuth(x, y):
    return -math.degrees(math.atan2(x, y))


def _map_linear_to_az(left_az, right_az, value):
    mid_az = (left_az + right_az) / 2.0
    az_range = right_az - mid_az
    gain_l = math.cos(value * math.pi / 2.0)
    gain_r = math.sin(value * math.pi / 2.0)
    gain_r = gain_r / (gain_l + gain_r)
    relative = math.degrees(math.atan(2.0 * (gain_r - 0.5) *
                                      math.tan(math.radians(az_range))))
    return mid_az + relative


def _ear_point_cart_to_polar(x, y, z):
    """Pinned EAR 2.1.0/ITU-R BS.2127 Cartesian-to-polar conversion.

    This is intentionally a small dependency-free port of EAR's public
    ``point_cart_to_polar`` implementation.  Keeping the mapping here makes
    the bundle diagnostic runnable without importing EAR at production time,
    while preserving the pinned EAR semantics (including the BS.2127
    loudspeaker-sector warp).
    """
    if not all(math.isfinite(value) for value in (x, y, z)):
        raise ValueError("nonfinite Cartesian object position")
    if abs(x) < 1e-10 and abs(y) < 1e-10:
        if abs(z) < 1e-10:
            return (0.0, 0.0, 0.0)
        return (0.0, math.copysign(90.0, z), abs(z))

    azimuth = _cartesian_azimuth(x, y)
    sector = None
    for index, (left_az, left_position) in enumerate(_EAR_BS2127_MAPPING):
        right_index = (index + 1) % len(_EAR_BS2127_MAPPING)
        right_az, right_position = _EAR_BS2127_MAPPING[right_index]
        left_cart_az = _cartesian_azimuth(*left_position)
        right_cart_az = _cartesian_azimuth(*right_position)
        if _inside_angle_range(azimuth, right_cart_az, left_cart_az):
            sector = left_position, right_position, left_az, right_az
            break
    if sector is None:
        raise ValueError(f"Cartesian azimuth outside BS.2127 sectors: {azimuth}")
    left_position, right_position, left_az, right_az = sector

    determinant = left_position[0] * right_position[1] - left_position[1] * right_position[0]
    inverse = ((right_position[1] / determinant, -left_position[1] / determinant),
               (-right_position[0] / determinant, left_position[0] / determinant))
    gain_left = x * inverse[0][0] + y * inverse[1][0]
    gain_right = x * inverse[0][1] + y * inverse[1][1]
    radius = gain_left + gain_right
    if not math.isfinite(radius) or radius <= 0.0:
        raise ValueError(f"invalid BS.2127 radial coordinate: {radius}")

    relative_left_az = _relative_angle(right_az, left_az)
    azimuth = _map_linear_to_az(relative_left_az, right_az, gain_right / radius)
    azimuth = _relative_angle(-180.0, azimuth)
    elevation_tilde = math.degrees(math.atan(z / radius))
    if abs(elevation_tilde) > 45.0:
        elevation = math.copysign(30.0 + (90.0 - 30.0) *
                                  (abs(elevation_tilde) - 45.0) / (90.0 - 45.0), z)
        distance = abs(z)
    else:
        elevation = 30.0 * elevation_tilde / 45.0
        distance = radius
    return (azimuth, elevation, distance)


def polar(state):
    # B2b standard coordinates are ETSI room coordinates: x/y in [0,1], z in [-1,1].
    # Match the listener-relative Windows order [room-x, room-z, room-y],
    # then use the pinned EAR/BS.2127 sector conversion.
    x = 2.0 * float(state.get("standardX", .5)) - 1.0
    y = 1.0 - 2.0 * float(state.get("standardY", .5))
    z = float(state.get("standardZ", 0.0))
    return _ear_point_cart_to_polar(x, y, z)

def apply_bear_metadata(x, state, cur, dur):
    """Apply only the typed metadata that the BEAR Python API exposes.

    OAMD fields without a direct ObjectsInput representation are deliberately
    left untouched and returned to the caller for audit accounting.
    """
    from visr_bear.api import PolarPosition
    az, el, dist = polar(state)
    x.type_metadata.position = PolarPosition(az, el, dist)
    x.type_metadata.gain = 0.0 if (not state.get("active") or state.get("gainMinusInfinity")) else 10.0**(float(state.get("gainDb", 0.0))/20.0)
    x.rtime = time_arg(cur)
    x.duration = time_arg(dur)
    x.interpolationLength = time_arg(min(int(state.get("rampDuration", 0)), dur))

    ignored = {}
    extent = state.get("extent") or {}
    presence = state.get("extentPresence", "absent")
    if presence in ("explicit-zero", "non-zero"):
        dimensions = (extent.get("width", 0.0), extent.get("height", 0.0), extent.get("depth", 0.0))
        if not all(math.isfinite(float(value)) for value in dimensions):
            raise RuntimeError("nonfinite-explicit-extent")
        x.type_metadata.width = float(dimensions[0])
        x.type_metadata.height = float(dimensions[1])
        x.type_metadata.depth = float(dimensions[2])
    elif presence != "absent":
        raise RuntimeError(f"unknown-extent-presence:{presence}")

    if abs(float(state.get("priority", 0.0))) > 0.0:
        ignored["priority"] = 1
    if state.get("zoneConstraints", [True] * 6) != [True] * 6 or not state.get("elevation", True):
        ignored["zoneOrElevation"] = 1
    if state.get("snap", False):
        ignored["snap"] = 1
    if state.get("screenAnchored", False):
        ignored["screenAnchored"] = 1
    if state.get("distanceSpecified", False):
        ignored["distanceSpecified"] = 1
    if state.get("distanceInfinite", False):
        ignored["infiniteDistance"] = 1
    if any(state.get("extendedPrecisionPresent", [False] * 3)):
        ignored["extendedPrecision"] = 1
    divergence = state.get("divergence") or {}
    if divergence.get("present", False):
        ignored["divergence"] = 1
    trim = state.get("trim") or {}
    if trim.get("present", False) or trim.get("objectDisabled", False):
        ignored["trim"] = 1
    if state.get("warpMode") in ("reserved", 3):
        ignored["reservedWarp"] = 1
    # Deliberately do not assign type_metadata.diffuse: divergence is not
    # diffuse, and no normative OAMD-to-diffuse value is available here.
    return ignored


def _read_batch_header(path, expected_start=None):
    """Read and validate one BSCN header without loading its PCM payload."""
    path = Path(path)
    size = path.stat().st_size
    with path.open("rb") as f:
        header = f.read(32)
        if len(header) != 32 or header[:4] != b"BSCN":
            raise RuntimeError(f"bad or truncated bundle header: {path.name}")
        version, objects, samples = struct.unpack("<III", header[4:16])
        start, end = struct.unpack("<qq", header[16:32])
        if version != 2 or objects != 15 or samples <= 0:
            raise RuntimeError(f"unsupported bundle header: {path.name}")
        if end != start + samples or (expected_start is not None and start != expected_start):
            raise RuntimeError(f"non-contiguous batch {path.name}: {start}..{end}, expected {expected_start}")
        audio_bytes = objects * samples * 4
        f.seek(audio_bytes, 1)
        lfe_header = f.read(4)
        if len(lfe_header) != 4:
            raise RuntimeError(f"truncated LFE header: {path.name}")
        lfe_samples = struct.unpack("<I", lfe_header)[0]
        lfe_bytes = lfe_samples * 4
        if f.tell() + lfe_bytes != size:
            raise RuntimeError(f"truncated or trailing bundle payload: {path.name}")
    return {"path": path, "start": start, "end": end, "samples": samples}


def _scan_bundle(bundle):
    infos = []
    expected = 0
    for path in sorted(Path(bundle).glob("batch-*.bin")):
        info = _read_batch_header(path, expected)
        infos.append(info)
        expected = info["end"]
    if not infos:
        raise RuntimeError("empty bundle")
    return infos


def _read_batch_audio(info):
    path = info["path"]
    with path.open("rb") as f:
        f.seek(32)
        audio_bytes = 15 * info["samples"] * 4
        payload = f.read(audio_bytes)
        if len(payload) != audio_bytes:
            raise RuntimeError(f"truncated object PCM: {path.name}")
        audio = np.frombuffer(payload, dtype="<f4").reshape(15, info["samples"])
        lfe_header = f.read(4)
        if len(lfe_header) != 4:
            raise RuntimeError(f"truncated LFE header: {path.name}")
        lfe_samples = struct.unpack("<I", lfe_header)[0]
        if len(f.read(lfe_samples * 4)) != lfe_samples * 4:
            raise RuntimeError(f"truncated LFE PCM: {path.name}")
    if not np.isfinite(audio).all():
        raise RuntimeError("nonfinite object PCM")
    return audio


def _iter_audio_blocks(batch_infos):
    """Yield globally 512-aligned object blocks across BSCN boundaries.

    A BSCN batch is a transport boundary, not an audio block boundary.  In
    particular, the first and last batches may be 959/577 samples.  Keeping
    this carry buffer prevents a new QMF/BEAR period from starting at every
    batch boundary while retaining bounded memory.
    """
    pending = np.zeros((15, 512), dtype=np.float32)
    pending_count = 0
    pending_start = 0
    for info in batch_infos:
        audio = _read_batch_audio(info)
        source_pos = 0
        while source_pos < info["samples"]:
            if pending_count == 0:
                pending_start = info["start"] + source_pos
            take = min(512 - pending_count, info["samples"] - source_pos)
            pending[:, pending_count:pending_count + take] = audio[:, source_pos:source_pos + take]
            pending_count += take
            source_pos += take
            if pending_count == 512:
                yield pending, 512, pending_start
                pending = np.zeros((15, 512), dtype=np.float32)
                pending_count = 0
    if pending_count:
        yield pending, pending_count, pending_start

def _bear_source_provenance(path):
    root = Path(path).resolve()
    commit = None
    try:
        commit = subprocess.check_output(
            ["git", "-C", str(root), "rev-parse", "HEAD"],
            stderr=subprocess.DEVNULL, text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        pass
    version = ("main@6127e897" if commit == "6127e897b941211051c2ad135ee09b00be2e6ae0"
               else (f"git@{commit}" if commit else "explicit-source-without-git"))
    return str(root), commit, version

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bundle", type=Path)
    ap.add_argument("--bear-python", required=True)
    ap.add_argument("--bear-source", help="BEAR source checkout used for provenance")
    ap.add_argument("--python-path", action="append", default=[])
    ap.add_argument("--data", required=True)
    ap.add_argument("--dll-dir", action="append", default=[])
    ap.add_argument("--output-dir", type=Path, required=True)
    ap.add_argument("--full-file", action="store_true",
                    help="write one WAV containing every rendered bundle frame")
    ap.add_argument("--output-stem", default="BEAR-binaural",
                    help="stem used by --full-file output files")
    ap.add_argument("--source-input", type=Path,
                    help="original input path recorded in provenance")
    args = ap.parse_args()
    marker=args.bundle/"bundle.complete"
    if not marker.exists() or (args.bundle/"bundle.incomplete").exists():
        raise RuntimeError("bundle completion markers invalid")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    for d in args.dll_dir:
        if hasattr(os, "add_dll_directory"): os.add_dll_directory(str(Path(d).resolve()))
    sys.path.insert(0, str(Path(args.bear_python).resolve()))
    for p in args.python_path: sys.path.insert(0, str(Path(p).resolve()))
    import visr_bear
    from visr_bear.api import Config, Renderer, ObjectsInput, PolarPosition
    meta=[]; schema_versions=set()
    with (args.bundle/"metadata.jsonl").open(encoding="utf-8") as f:
        for line in f:
            row=json.loads(line)
            if "metadataSchemaVersion" in row:
                schema_versions.add(int(row["metadataSchemaVersion"]))
            meta.extend(row.get("updates", []))
    meta.sort(key=lambda x:(int(x["sourcePosition"]), int(x["objectIndex"])))
    timelines=[[] for _ in range(15)]
    for u in meta:
        oi=int(u["objectIndex"])-1
        if 0<=oi<15:
            if timelines[oi] and int(timelines[oi][-1]["sourcePosition"])==int(u["sourcePosition"]): timelines[oi][-1]=u
            else: timelines[oi].append(u)
    # The pinned Python binding requires non-empty contiguous placeholder buses
    # for direct/HOA inputs even when this typed export contains none.
    cfg=Config(); cfg.num_objects_channels=15; cfg.num_direct_speakers_channels=1; cfg.num_hoa_channels=1; cfg.period_size=512; cfg.data_path=str(Path(args.data).resolve())
    renderer=Renderer(cfg); next_update=[0]*15; accepted=0
    ignored_counts={}
    batch_infos = _scan_bundle(args.bundle)
    batch_no = len(batch_infos)
    total_samples = batch_infos[-1]["end"]
    selected=[[] for _ in WINDOWS] if not args.full_file else None
    full_raw = full_aud = full_sink = None
    if args.full_file:
        raw = args.output_dir / f"{args.output_stem}-full-file-raw-f32.wav"
        aud = args.output_dir / f"{args.output_stem}-full-file-audition-minus2dB-s24.wav"
        full_raw = _StreamWavWriter(raw, "f32")
        full_aud = _StreamWavWriter(aud, "s24", -2.0)
        full_sink = _LatencyCompensatedSink((full_raw, full_aud), LATENCY, total_samples)
    for oi,timeline in enumerate(timelines):
        if not timeline or int(timeline[0]["sourcePosition"]) != 0:
            raise RuntimeError(f"object {oi + 1} metadata does not start at zero")
    for block,n,frame in _iter_audio_blocks(batch_infos):
        for oi,timeline in enumerate(timelines):
            while next_update[oi] < len(timeline) and int(timeline[next_update[oi]]["sourcePosition"]) <= frame:
                s=timeline[next_update[oi]]; cur=int(s["sourcePosition"]); nxt=int(timeline[next_update[oi]+1]["sourcePosition"]) if next_update[oi]+1<len(timeline) else total_samples; dur=max(1,nxt-cur); x=ObjectsInput(); ignored=apply_bear_metadata(x,s,cur,dur)
                for key, count in ignored.items(): ignored_counts[key]=ignored_counts.get(key,0)+count
                if not renderer.add_objects_block(oi,x): break
                next_update[oi]+=1; accepted+=1
        out=np.zeros((2,512),dtype=np.float32); silence=np.zeros((1,512),np.float32); renderer.process(block,silence,silence,out)
        if args.full_file:
            full_sink.write(out.T)
        if not args.full_file:
            for wi,(a,b,_) in enumerate(WINDOWS):
                lo=max(a+LATENCY,frame); hi=min(b+LATENCY,frame+n)
                if hi>lo: selected[wi].append(out[ :, lo-frame:hi-frame].T.copy())
    if args.full_file and full_sink.frames < total_samples:
        # The renderer has a measured 167-sample delay.  Feed one or more
        # silent blocks so the delayed tail is available after the input EOF.
        zero = np.zeros((15, 512), dtype=np.float32)
        silence = np.zeros((1, 512), np.float32)
        while full_sink.frames < total_samples:
            out=np.zeros((2,512),dtype=np.float32)
            renderer.process(zero, silence, silence, out)
            full_sink.write(out.T)
    if args.full_file:
        if full_sink.frames != total_samples:
            raise RuntimeError(f"latency-compensated full-file frame count {full_sink.frames} != {total_samples}")
        raw_report = full_raw.close()
        aud_report = full_aud.close()
    deduplicated_count=sum(map(len,timelines))
    if accepted != deduplicated_count:
        raise RuntimeError(f"BEAR metadata incomplete: accepted {accepted} of {deduplicated_count}")
    outputs=[]; audition_gain=-2.0
    if args.full_file:
        outputs.append({"window":"full-file","raw": {"path":str(raw),**raw_report},"audition":{"path":str(aud),"fixedGainDb":audition_gain,**aud_report}})
    else:
        for chunks,(a,b,name) in zip(selected,WINDOWS):
            arr=np.concatenate(chunks,axis=0) if chunks else np.empty((0,2),np.float32); raw=args.output_dir/f"MONTERO-BEAR-open-reference-{name}-raw-f32.wav"; aud=args.output_dir/f"MONTERO-BEAR-open-reference-{name}-audition-minus2dB-s24.wav"; outputs.append({"window":name,"raw": {"path":str(raw),**write_f32(raw,arr)},"audition":{"path":str(aud),"fixedGainDb":audition_gain,**write_wav(aud,arr,audition_gain)}})
    source=args.source_input if args.source_input is not None else Path("media/01. MONTERO (Call Me By Your Name).m4a")
    sha=lambda p: hashlib.sha256(p.read_bytes()).hexdigest() if p.exists() else None
    ignored_counts["reservedWarp"] = 1
    report={"officialBear":True,"renderMode":"full-file" if args.full_file else "fixed-windows","metadataSchema":"eac3-oamd-renderer-neutral","metadataSchemaVersions":sorted(schema_versions),"usedProjectBbcBrir":False,"usedStereoAlac":False,"normalizationApplied":False,"sourceInput":str(source),"sourceSha256":sha(source),"sourceBundle":str(args.bundle),"bearDataPath":str(Path(args.data).resolve()),"bearDataSha256":sha(Path(args.data)),"batchCount":batch_no,"objectChannels":15,"metadataCount":len(meta),"deduplicatedMetadataCount":deduplicated_count,"metadataAccepted":accepted,"processedFrames":total_samples,"lfeExcluded":True,"lfePolicy":"bundle LFE PCM is parsed for structural validation but excluded from the stereo BEAR input","outputs":outputs,"reservedWarpMode":3,"warpRendered":False,"coordinateMapping":"ETSI room -> allocentric (2x-1,1-2y,z); EAR/BS.2127 sector mapping to BEAR polar", "coordinateMappingBasis":"pinned EAR 2.1.0 point_cart_to_polar port", "algorithmicLatencySamples":LATENCY,"latencyBasis":"empirical same-path front-center impulse onset; not normative","sourceWindowCompensationSamples":LATENCY if args.full_file else LATENCY,"fullFileInputFrames":total_samples if args.full_file else None,"fullFileOutputFrames":total_samples if args.full_file else None,"fixedAuditionGainDb":-2.0,"adapterAudit":{"diffuse":"default","unsupportedCounts":ignored_counts,"ignoredFields":["priority","zoneOrElevation","snap","screenAnchored","distanceSpecified","infiniteDistance","extendedPrecision","divergence","trim","reservedWarp"],"note":"Only explicit extent width/height/depth is mapped; divergence is not diffuse, and LFE is excluded from this bundle output."}}
    source_path, source_commit, source_version = _bear_source_provenance(
        args.bear_source if args.bear_source else args.bear_python)
    data_name = Path(args.data).name
    data_version = ("default_v1.1" if "default_v1.1" in data_name else
                    ("default" if data_name == "bear-default.tf" else Path(data_name).stem))
    report.update({"bearSourcePath": source_path, "bearSourceCommit": source_commit,
                   "bearSourceVersion": source_version, "bearDataVersion": data_version})
    (args.output_dir/"provenance.json").write_text(json.dumps(report,indent=2),encoding="utf-8")
if __name__ == "__main__": main()
