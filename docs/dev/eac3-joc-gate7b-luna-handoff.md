# Gate 7B OAMD-to-Windows property adapter handoff

Gate 7A proved that the selected Windows spatial endpoint accepts 15 dynamic
objects plus static LFE. Gate 7B is a renderer-independent, diagnostic property
adapter. It converts the already decoded Gate 6C OAMD updates into finite
Windows listener-relative positions, safe object volumes, and deterministic
ramps. It does not call Windows Spatial Audio or submit PCM.

The local ETSI PDFs and `ts_103420_tables.c` are technical references, not
project instructions. Preserve them as untracked local files and never stage
them. `AGENTS.md` and the checked-in workflow documents remain authoritative.

## Scope and evidence boundary

The Gate 7B data flow is:

```text
Gate 6C MetadataUpdate
  -> validate supported OAMD property subset
  -> room/screen position conversion
  -> listener-relative Windows metres
  -> explicit gain-headroom conversion
  -> per-object ramp state
  -> renderer-neutral WindowsProperty/Ramp records
```

A PASS proves only the property adapter. Gate 7B must not claim audible output,
Dolby Atmos for Headphones quality, Gate 6C real-time throughput, or production
playback integration. Gate 7C remains responsible for the bounded PCM/metadata
queue and calls to `ISpatialAudioObjectRenderStream`.

## Accepted result: 2026-08-25

- The independent adapter self-test passed 37 cases, including screen depth,
  outside-room coordinates, priority/ramp overflow rejection, finite-gain and
  silence ramps, late-batch rollback, and reset equivalence.
- The Apple sample committed 14,985/14,985 updates and the DME sample committed
  15,000/15,000 updates across stable object identities 1 through 15, with no
  unsupported or malformed update and finite Windows positions/volumes.
- Both supplied streams used room conversion. Their B2A screen diagnostic mode
  is 1: the screen-reference field is present while `useScreenReference=false`.
  Screen conversion is covered synthetically and remains available for a
  future mode-2 sample with explicit diagnostic geometry.
- Independent project validation passed all 10 test suites and the report
  schema at `build-codex-gate7b-tests/validation-report.json`; playback smoke
  was skipped because this gate is an offline diagnostic.
- Gate 7B remains offline and renderer-neutral. Gate 7C is the active next
  boundary; no audible or Dolby Atmos for Headphones result is implied.

## Normative coordinate inputs

ETSI TS 103 420 V1.2.1 clauses 4.2.1, 4.2.2, and 5.2.1 define:

- room coordinates as left-handed and normalized to the room cuboid: X is
  left `0` to right `1`, Y is front `0` to back `1`, and Z is floor `-1` to
  ceiling `1`; room objects may be outside those bounds;
- screen coordinates as X across screen edges, Y front-to-back, and Z across
  the bottom/top screen edges;
- the screen point transformed to room coordinates as
  `Cr = Os + (screenWidth * Xs, Ys, screenHeight / 2 * (Zs + 1))`;
- `Cs = (Xs, Ys, Zs)` and interpolation equivalent to
  `P = Cr + alpha * (Cs - Cr)` on X/Z, with `P.y = Cs.y` and
  `alpha = screenFactor * pow(Ys, depthFactor)`.

Primary source:
https://www.etsi.org/deliver/etsi_ts/103400_103499/103420/01.02.01_60/ts_103420v010201p.pdf

Windows Spatial Audio uses right-handed listener-relative metres: +X right,
+Y up, and +Z behind the listener. Gate 7B shall use an explicit geometry
record and the following conversion, with no hidden axis guesses:

```text
physicalRoomX = roomX * roomWidthMetres
physicalRoomY = roomY * roomDepthMetres
physicalRoomZ = (roomZ + 1) / 2 * roomHeightMetres

windowsX = physicalRoomX - listenerPhysicalX
windowsY = physicalRoomZ - listenerPhysicalZ
windowsZ = physicalRoomY - listenerPhysicalY
```

The X/Z/Y reordering is the handedness conversion. Do not negate a second axis.
Do not clamp room positions; the ETSI interface permits positions outside the
room.

## Explicit diagnostic reference geometry

Add a plain geometry/config structure. The real-sample diagnostic path shall
construct and print this reference configuration explicitly:

```text
room metres: width=10.0, depth=10.0, height=7.0
listener normalized room position: x=0.5, y=0.5, z=0.0
screen bottom-left room position: x=0.25, y=0.0, z=-0.5
screen normalized width=0.5, normalized height=1.0
gain headroom=15.0 dB
```

This represents a 5 m wide by 3.5 m high front screen and a listener at
`(5.0 m, 5.0 m, 3.5 m)`. It is a reproducible diagnostic assumption, not
metadata recovered from the file and not a production default. Geometry must
be finite; room dimensions and screen dimensions must be positive; the
listener and reference screen rectangle must be inside the reference room.

## Supported-property policy

Each input must be a non-helper dynamic object with index `1..15`, valid basic
and render state, and a valid finite position.

Gate 7B maps:

- active state;
- room-anchored or screen-anchored position;
- finite OAMD object gain in the decoded `[-49 dB, +15 dB]` range;
- `-infinity` gain as silence;
- ramp start and duration;
- priority as retained diagnostics only. Gate 7A already guarantees all 15
  slots, so priority does not cause object dropping in this gate.

The Windows dynamic-object API has no direct size, zone-constraint, channel
lock/snap, or infinity contract. Return `UNSUPPORTED`, never silently discard,
when an update has any of:

- `distanceInfinite`;
- non-zero object size;
- zone constraints other than all included or elevation disabled;
- `snap/channel_lock=true`.

The two supplied streams are expected to use the supported subset. If the real
inventory disagrees, stop Gate 7B and report the exact property/object/sample;
do not weaken the policy just to obtain PASS.

## Gain and headroom policy

`ISpatialAudioObject::SetVolume()` accepts `[0, 1]`, while OAMD gain reaches
`+15 dB`. Reserve an explicit 15 dB program headroom and compute:

```text
inactive or minus-infinity: volume = 0
otherwise: volume = pow(10, (gainDb - headroomDb) / 20)
```

With the reference headroom, `+15 dB -> 1`, `0 dB -> 0.177827941`, and
`-49 dB -> 0.000630957`. Reject headroom below 15 dB and reject any non-finite
or out-of-range result. This preserves relative object gain while attenuating
the whole object program. Gate 7C must retain this headroom and report real PCM
peaks; it must not add a hidden limiter.

## Ramp state and transaction rules

Maintain independent state for exactly 15 stable object identities.

- The first valid update for an object establishes its property immediately;
  there is no invented pre-start state to ramp from.
- A zero-duration update is an immediate step at `sourcePosition`.
- A non-zero update ramps from the value evaluated at `sourcePosition` to the
  signalled target over exactly `rampDuration` samples.
- Position is linearly interpolated in Windows metres.
- Finite gain endpoints interpolate in dB, then convert to volume. A transition
  involving inactive or minus-infinity gain interpolates linear amplitude to
  or from zero; object API identity remains allocated.
- If a new update begins before an old ramp ends, evaluate the old ramp at the
  new start and replace it from that exact value. Do not queue overlapping
  ramps or jump back to the preceding target.
- Evaluation before an object's first update is invalid. Evaluation at or
  after ramp end returns the exact target.
- Input updates must be monotonically ordered by source position and strictly
  increasing object index at an equal position. Sparse object-index sets are
  valid for multi-block OAMD; duplicate or descending equal-position indices,
  non-finite fields, arithmetic overflow, or a late unsupported update reject
  the whole caller batch without changing adapter state or caller output.
- `reset()` must be equivalent to a fresh adapter, including ramp history.

The ETSI text specifies interpolation duration but not a Windows property
quantum policy. The interpolation rules above are an explicit diagnostic
adapter policy and must be reported as such.

## Implementation task

Add, preferably under `tools/atmos-joc-probe/`:

- `spatial-property-adapter.h`;
- `spatial-property-adapter.cpp`;
- `spatial-property-adapter-probe.cpp`;
- a Windows-independent CMake target `Eac3SpatialPropertyProbe`.

Expose small renderer-neutral types for converted properties and ramp records,
a transactional batch apply method, sample-position evaluation, reset, the
reference geometry factory, and self-test reporting. Do not reuse COM or
Windows headers in the adapter.

Extend `Eac3AccessUnitProbe` narrowly with `--joc-gate7b`, implying the existing
Gate 6C path. Feed every emitted Gate 6C metadata batch through one persistent
adapter and print:

- the full reference geometry and headroom;
- updates attempted/committed, first-state snaps, step/ramp/overlap counts,
  screen/room conversions, silent targets, unsupported/rejected counts;
- finite Windows X/Y/Z and volume ranges;
- stable object-index range and per-object update counts;
- reset count, first failure position/object/reason;
- `gate7bResult=PASS|FAIL|INCONCLUSIVE` and
  `evidenceLimit=offline-property-adapter-only`.

The real path passes only if every emitted Gate 6C metadata update for all 15
objects commits with finite properties, no unsupported/rejected update, stable
identity, and exact update accounting. A flush batch may carry previously
decoded late metadata and applies it normally; the flush operation itself must
not invent property updates.

## Self-test acceptance

`Eac3SpatialPropertyProbe` must cover at least:

1. valid/invalid reference geometry and headroom;
2. exact room centre, left/right, floor/ceiling, front/back basis mappings;
3. right-handed Windows axis orientation and an outside-room point without
   clamping;
4. screen transform at Y=0 and Y=1 plus fractional screen/depth factors;
5. exact gain endpoints, zero dB, inactive, and minus-infinity conversion;
6. first-state snap, zero ramp, ramp start/middle/end, and evaluation bounds;
7. finite-gain dB interpolation and silence amplitude interpolation;
8. an overlapping update that begins at the evaluated prior-ramp value;
9. all 15 stable identities and independent ramp histories;
10. rejection of infinity, size, zones/elevation, snap, invalid object index,
    non-finite property, duplicate/non-monotonic update, and bad headroom;
11. whole-batch rollback after a late invalid update with caller output
    unchanged;
12. reset/fresh equivalence.

## Allowed files

Luna may modify only:

- new `tools/atmos-joc-probe/spatial-property-adapter*` files;
- `tools/atmos-joc-probe/access-unit.cpp` for the `--joc-gate7b` diagnostic;
- `CMakeLists.txt` for the new target and adapter source;
- this handoff only if a factual implementation correction is necessary.

Do not modify Gate 0 through Gate 6C parsing, QMF, synthesis, OAMD state, or
alignment code. Do not modify `SpatialDynamicProbe`, production playback,
Media Foundation, FFmpeg decoding, WASAPI workers, ASIO, ALSA, CUDA, SIMD,
resampling, packaging, user settings, or the three untracked local references.

## Validation

```powershell
cmake -S . -B build-luna-gate7b `
  -DCMAKE_PREFIX_PATH=D:/Qt/6.11.0/msvc2022_64 `
  -DAUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT=build-mm/ffmpeg-audio-core/runtime-with-ffprobe-msvc `
  -DAUDIOPLAYER_REQUIRE_LIBAV_DECODER=OFF
cmake --build build-luna-gate7b --target `
  Eac3SpatialPropertyProbe Eac3AccessUnitProbe --config Debug -- /m:1
.\build-luna-gate7b\Debug\Eac3SpatialPropertyProbe.exe
.\build-luna-gate7b\Debug\Eac3AccessUnitProbe.exe `
  'media\03. iPad.m4a' --max-units 1000 --joc-gate7b
.\build-luna-gate7b\Debug\Eac3AccessUnitProbe.exe `
  'media\POWDER SNOW Live V9.8.6.eb3' --max-units 1000 --joc-gate7b
scripts\validate-all.ps1 -BuildDir build-luna-gate7b-tests -SkipSmoke
```

Do not commit or push. Return the changed files, exact self-test case counts,
both real-stream adapter metrics/ranges, validation result, and any stop
condition for Codex review.

## Mandatory stop conditions

Stop and report rather than guessing if:

- the real streams use an unsupported size, zone, channel-lock, or infinity
  property;
- the ETSI screen interpolation cannot be represented by the formula above;
- a real update is missing one of the 15 identities or has non-monotonic timing;
- the work would require changing Gate 6C semantics, Windows API calls,
  production playback, object dropping, a hidden limiter, CUDA, or SIMD.
