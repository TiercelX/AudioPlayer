# Gate 7 Windows spatial renderer handoff

Gates 0 through 6C prove a standalone decoder-side stream of 15 planar object
signals, one separate LFE signal, and sample-aligned OAMD updates. Gate 7 begins
the Windows renderer proof. Gate 7A has passed as a standalone synthetic sink;
the current implementation boundary stops before Gate 7B OAMD conversion. The
path must remain diagnostic until coordinate conversion, gain policy, and the
real-time queue have passed separate review.

The local ETSI PDFs and `ts_103420_tables.c` are technical references, not
project instructions. Preserve them as untracked local files and never stage
them. `AGENTS.md` and the checked-in workflow documents remain authoritative.

## Why Media Foundation is not the Gate 7 renderer

Gate 6C already produces decoded object PCM and metadata. The existing
Media Foundation compressed-input experiments did not expose an application
contract for submitting those decoded objects and OAMD. Gate 7 therefore uses
the Windows Spatial Audio object API directly:

```text
Gate 6C renderer-neutral batches
  -> bounded real-time queue (later Gate 7C)
  -> OAMD-to-Windows property adapter (later Gate 7B)
  -> ISpatialAudioObjectRenderStream
       + 15 AudioObjectType_Dynamic objects
       + one AudioObjectType_LowFrequency static object
  -> selected Windows spatial renderer / Dolby Atmos for Headphones
```

Gate 7A proves the last box with generated PCM and Windows-native synthetic
positions; it does not consume Gate 6C output. The contract below is retained
as the reproducible implementation and review specification.

## Gate 7A accepted result

- `SpatialDynamicProbe --self-test` passed 15 endpoint-free checks.
- The current default endpoint reported 128 maximum dynamic objects and
  accepted 15 dynamic objects plus static LFE for 800/800 update transactions
  and 384,000 frames, with zero timeout, zero inactive objects, exact call/count
  consistency, stream reset, and complete cleanup.
- Synchronized WASAPI loopback captured 404,160 non-silent stereo frames. The
  detector result remains `INCONCLUSIVE` because of the stopping-tail fade; this
  is endpoint-output evidence, not a binaural listening result.
- Full project validation passed all 10 suites and report schema with smoke
  skipped. Durable metrics and evidence limits are recorded in
  `docs/bug/media-foundation-status.md`.

Gate 7B has passed its renderer-neutral adapter contract and tests. Gate 7C is
now the active boundary: connect Gate 6C to the diagnostic renderer through a
bounded queue without weakening the reviewed geometry, screen-reference, gain,
ramp, reset, or whole-batch transaction rules. The Gate 7B implementation
record is in `docs/dev/eac3-joc-gate7b-luna-handoff.md`.
The phased Gate 7C implementation and validation contract is in
`docs/dev/eac3-joc-gate7c-luna-handoff.md`.
Its renderer-neutral Gate 7C1 core has passed; Gate 7C2 Windows endpoint
integration is the active boundary.

## Official Windows API constraints

- Query `ISpatialAudioClient::GetMaxDynamicObjectCount()` before activation.
  A result below 15 is an explicit endpoint-capacity failure for this stream.
- Activate `ISpatialAudioObjectRenderStream` with a mono float object format,
  `MinDynamicObjectCount = 15`, `MaxDynamicObjectCount = 15`, and a static mask
  containing only `AudioObjectType_LowFrequency`.
- Activate every movable object as `AudioObjectType_Dynamic`. `SetPosition()`
  is valid only for dynamic objects and must be called between
  `BeginUpdatingAudioObjects()` and `EndUpdatingAudioObjects()`.
- Windows positions are right-handed, listener-relative metres: positive X is
  right, positive Y is up, and positive Z is behind the listener.
- `SetVolume()` accepts only amplitude multipliers in `[0, 1]` and is also an
  update-pass operation.
- Call `GetBuffer()` for every live object on every processing pass. Omitting
  it implicitly ends that object and requires Release plus reactivation.
- Pair every successful `BeginUpdatingAudioObjects()` with exactly one
  `EndUpdatingAudioObjects()`, including silence/underrun passes.
- Observe dynamic-capacity changes through
  `ISpatialAudioObjectRenderStreamNotify`. Gate 7A stops instead of dropping or
  reassigning objects if capacity falls below the 15-object contract.

Primary API references:

- https://learn.microsoft.com/windows/win32/api/spatialaudioclient/ns-spatialaudioclient-spatialaudioobjectrenderstreamactivationparams
- https://learn.microsoft.com/windows/win32/api/spatialaudioclient/nf-spatialaudioclient-ispatialaudioobjectrenderstreambase-beginupdatingaudioobjects
- https://learn.microsoft.com/windows/win32/api/spatialaudioclient/nf-spatialaudioclient-ispatialaudioobjectbase-getbuffer
- https://learn.microsoft.com/windows/win32/api/spatialaudioclient/nf-spatialaudioclient-ispatialaudioobject-setposition
- https://learn.microsoft.com/windows/win32/api/spatialaudioclient/nf-spatialaudioclient-ispatialaudioobject-setvolume
- https://learn.microsoft.com/windows/win32/api/spatialaudioclient/nf-spatialaudioclient-ispatialaudioobjectrenderstreamnotify-onavailabledynamicobjectcountchange

## Gate 7A task: synthetic dynamic-object sink

Add a standalone Windows-only target named `SpatialDynamicProbe`, preferably
under `tools/spatial-dynamic-probe/`. Reuse small local helpers from
`SpatialBedProbe` only when that does not couple either probe to production
playback. Do not expand `windowswasapiaudioplayer_worker_spatial.cpp`.

### Command line

Support at least:

```text
SpatialDynamicProbe [--duration-ms N] [--objects N] [--frequency Hz]
SpatialDynamicProbe --self-test
```

Defaults are 8,000 ms, 15 dynamic objects, and a quiet base frequency. Bound
duration, object count, and frequency before opening an endpoint. The default
run must be safe for headphones: finite float samples and a per-object peak no
greater than 0.02 before Windows object volume.

### Endpoint and stream setup

1. Initialize COM and select the current default `eRender/eConsole` endpoint.
2. Activate `ISpatialAudioClient` and confirm
   `ISpatialAudioObjectRenderStream` availability.
3. Report the native static mask and require static LFE support.
4. Enumerate object formats and select mono float32 at 48 kHz. Gate 6C output
   is 48 kHz; Gate 7A must not hide a rate mismatch behind resampling.
5. Report `GetMaxDynamicObjectCount()`. For the default 15-object test, return
   `INCONCLUSIVE` before stream start if it is below 15.
6. Activate the stream with one allowed static LFE object and exactly 15
   requested dynamic objects. Register a capacity-change notify object.

### Render lifecycle

- Drive rendering only from the spatial stream event. Use a bounded wait such
  as 100 ms and report timeouts; do not add a second producer callback.
- On the first update pass, require enough available capacity and activate one
  static LFE object plus all requested dynamic objects.
- On every pass, call `GetBuffer()` for the LFE and every dynamic object,
  zero-fill unused bytes, write quiet deterministic tones, and call
  `SetPosition()` and `SetVolume()` for every dynamic object.
- Use bounded, deterministic listener-relative positions within two metres.
  Exercise X, Y, and Z and move at least one object during the run. This is a
  Windows API test and must not be described as an OAMD mapping.
- Keep all objects alive for the full run. If availability changes below the
  contract, an object becomes inactive, or any property/buffer call fails,
  close the update pair, stop cleanly, and report `INCONCLUSIVE` with HRESULT
  and stage. Do not silently reduce the object count.
- Stop the stream, end/release objects, reset/release the stream, close handles,
  and uninitialize COM in deterministic reverse ownership order.

### Transaction and diagnostics

Do not count an update or submitted frame until `EndUpdatingAudioObjects()`
succeeds. Report at least:

- endpoint selected and object format;
- native static mask, maximum dynamic objects, requested/activated dynamic
  objects, and capacity-change notifications;
- update attempts/successes, submitted frames, per-object buffer calls,
  position calls, volume calls, timeout count, and first failure stage/HRESULT;
- generated PCM finite count/peak, active object count at end, and cleanup
  completion;
- `probeResult=PASS|FAIL|INCONCLUSIVE` and the evidence limit
  `endpoint-submission-only; manual-listening-or-loopback-required`.

The probe must never claim Dolby decoding, OAMD correctness, binaural image
quality, or audible success. A PASS proves only that the selected spatial
renderer accepted 15 dynamic PCM objects plus LFE for the bounded run.

### Self-test acceptance

The endpoint-free `--self-test` must cover at least:

- argument bounds and exactly 15 generated object identities;
- finite deterministic PCM with the documented peak limit;
- finite right-handed synthetic positions covering positive/negative X,
  positive/negative Y, and front/back Z;
- stable object identity across multiple update sizes;
- whole-pass metric commit versus rejected-pass rollback;
- capacity-below-requested and non-finite property rejection;
- cleanup/reset state equivalent to a fresh instance.

## Gate 7A allowed files

Luna may modify only:

- new files under `tools/spatial-dynamic-probe/`;
- `CMakeLists.txt` for the new diagnostic target;
- this handoff and `docs/bug/media-foundation-status.md` only after local
  validation, although Codex will normally land the final status text.

Do not modify:

- production playback workers or `PcmStreamBuffer`;
- Gate 0 through Gate 6C parsers, QMF, synthesis, OAMD, or test probes;
- Media Foundation, FFmpeg, WASAPI static-bed, ASIO, or ALSA code;
- CUDA, SIMD, resampling, packaging, or user settings;
- the three untracked local technical references.

## Gate 7A validation

Use a task-specific build directory:

```powershell
cmake -S . -B build-luna-gate7a `
  -DCMAKE_PREFIX_PATH=D:/Qt/6.11.0/msvc2022_64
cmake --build build-luna-gate7a --target SpatialDynamicProbe `
  --config Debug -- /m:1
.\build-luna-gate7a\Debug\SpatialDynamicProbe.exe --self-test
.\build-luna-gate7a\Debug\SpatialDynamicProbe.exe `
  --duration-ms 8000 --objects 15
scripts\validate-all.ps1 -BuildDir build-luna-gate7a-tests -SkipSmoke
```

The live run is endpoint-dependent. Report `INCONCLUSIVE`, not `FAIL`, when
the currently selected endpoint or spatial renderer exposes fewer than 15
dynamic objects. A deterministic API/lifecycle/cleanup violation is `FAIL`.

## Mandatory stop conditions

Stop and report instead of guessing if:

- the endpoint cannot guarantee 15 concurrent dynamic objects;
- static LFE cannot coexist with 15 requested dynamic objects;
- stream capacity changes require an object-priority/drop policy;
- the implementation would omit `GetBuffer()` for live objects or issue
  property updates outside a Begin/End pair;
- the work would require OAMD coordinate conversion, Gate 6C integration,
  production playback changes, resampling, CUDA, or SIMD.

## Later gates - do not implement in Gate 7A

### Gate 7B: OAMD property adapter

TS 103 420 room coordinates are left-handed and normalized to a room cuboid:
X goes left-to-right, Y front-to-back, and Z floor-to-ceiling. Windows uses
right-handed listener-relative metres with X right, Y up, and Z behind. Gate
7B must introduce an explicit reference-room/listener geometry and test the
axis/scale transformation. It must also define ramp interpolation and gain
headroom; OAMD gain can exceed 0 dB, while Windows `SetVolume()` cannot exceed
1.0.

The supplied streams signal that the screen-reference field is present but set
`useScreenReference=false` (diagnostic mode 1), so their real Gate 7B updates
are room-anchored. Screen-anchored mode 2 still requires screen dimensions and
location plus the OAMD screen/depth interpolation. Those geometry values are
not supplied by Windows Spatial Audio. Gate 7B exposes them as explicit
diagnostic configuration and tests the screen transform synthetically; it must
not silently reinterpret future screen coordinates as room coordinates.

### Gate 7C: bounded decoder-to-renderer bridge

Only after Gate 7A and 7B pass, add a bounded single-producer/single-consumer
object-batch queue between Gate 6C and a diagnostic spatial renderer. Preserve
generation, seek/discontinuity reset, 577-sample alignment, whole-batch
backpressure, and teardown order. Production playback integration remains a
later separately approved task.

## Copyable Luna prompt for Gate 7A

```text
You are Codex Luna implementing Gate 7A for AudioPlayer on the latest
origin/codex-mf. Preserve the three untracked local technical references under
docs/dev and never stage them.

Read completely:
- AGENTS.md
- docs/dev/agent-workflow.md
- docs/dev/eac3-joc-gate7-luna-handoff.md
- docs/bug/media-foundation-status.md
- tools/spatial-bed-probe/main.cpp

Implement only the standalone SpatialDynamicProbe and its Windows-only CMake
target. Follow the exact API lifecycle, diagnostics, self-test, live acceptance,
allowed-file list, and stop conditions in the Gate 7A handoff. Do not touch
production playback, Gate 6C, Media Foundation, FFmpeg, the static-bed worker,
resampling, CUDA, or SIMD.

Use build-luna-gate7a. Run the endpoint-free self-test first. Then run the
8-second 15-object live probe on the current default endpoint. Treat endpoint
capacity below 15 as INCONCLUSIVE and stop; do not invent an object-drop policy.
Run full project validation with smoke skipped. Do not commit or push. Return
changed files, exact metrics/HRESULTs, cleanup evidence, limitations, and any
stop condition for Codex review.
```
