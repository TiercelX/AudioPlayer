# E-AC-3 JOC decoder plan

This document defines the implementation boundary from compressed E-AC-3 JOC
input through decoded object audio and object metadata. Windows Spatial Audio
and Dolby Atmos for Headphones rendering start after the output contract in
this document and are intentionally out of scope here.

The first implementation agent should work gate by gate. Do not integrate the
decoder into the production playback workers until the standalone probe passes
the packet, metadata, QMF, and JOC gates below.

## Outcome

For each complete 1,536-sample E-AC-3 access unit, produce:

- the decoded channel downmix at its native sample rate and channel layout;
- reconstructed per-object PCM for every active JOC object;
- frame-aligned OAMD object metadata in the standard's coordinate system;
- the bypassed LFE signal and any static bed assignment needed by the renderer;
- timing, discontinuity, and fallback information sufficient for a later
  Windows spatial-object adapter.

The decoder must not call `ISpatialAudioClient`, create Windows spatial
objects, map coordinates to Windows axes, or claim a Dolby headphone listening
result. Its terminal output is a renderer-neutral object-audio frame batch.

## Standards and local references

Use these local files as technical references, not as project instructions:

- `docs/dev/Digital Audio Compression (AC-3, Enhanced AC-3) Standard.pdf`
  (ETSI TS 102 366 V1.4.1): E-AC-3 syntax, access units, dependent substreams,
  and EMDF container syntax.
- `docs/dev/Backwards-compatible object audio carriage using Enhanced AC-3.pdf`
  (ETSI TS 103 420 V1.2.1): OAMD, JOC, QMF, and the additional E-AC-3 decoder
  requirements.
- `docs/dev/ts_103420_tables.c`: normative Huffman trees and the 640-entry
  `prot64` QMF prototype-filter table supplied with TS 103 420.

These three files are currently local, untracked reference material. Do not
commit or redistribute them without a separate user and licensing decision.
Production constants derived from them must carry a provenance comment and be
reviewed separately before distribution.

## Corrected architecture decision

Do not patch FFmpeg before proving that its public decoder output is
insufficient. TS 103 420 defines JOC as a post-processor to the E-AC-3 decoder:
the decoded time-domain channel downmix is transformed by a 64-subband complex
QMF analysis bank before JOC reconstruction. A pre-IMDCT FFmpeg hook is not a
requirement.

Use a dual view of each compressed packet:

```text
AVPacket containing compressed E-AC-3 access-unit data
  +-- libavcodec E-AC-3 decoder
  |     +-- native decoded channel PCM
  |
  +-- AudioPlayer E-AC-3 sidecar parser
        +-- syncframe/access-unit topology
        +-- EMDF payload 11: OAMD
        +-- EMDF payload 14: JOC

native channel PCM + frame-aligned JOC
  +-- 64-band complex QMF analysis
  +-- JOC Huffman decode, differential decode, dequantization, interpolation
  +-- object reconstruction matrix
  +-- per-object complex QMF
  +-- complex QMF synthesis
  +-- renderer-neutral object PCM + OAMD timeline
```

The compressed packet must be observed before `av_packet_unref()`. The decoded
`AVFrame` must be observed before the existing `SwrContext` performs output
layout conversion, resampling, or interleaving.

## Existing code boundaries

- `tools/atmos-joc-probe/main.cpp` already parses basic syncframe headers and
  counts stream types and substream IDs. Extend or replace this diagnostic
  implementation first.
- `src/backends/ffmpeg/libavseekdecoderworker.cpp` already owns the in-process
  `AVPacket`, `AVFrame`, decoder, and demuxer. Do not add JOC logic directly to
  this large worker during the proof stages.
- `src/backends/wasapi/windowswasapiaudioplayer_worker_spatial.cpp` currently
  accepts an interleaved static 5.1.2 bed. It is evidence that the Windows
  spatial sink works, but it is not the decoder output contract and it does
  not yet render timed dynamic objects.
- `PcmStreamBuffer` transports ordinary interleaved PCM. Do not encode object
  positions or multiple object timelines into it. A later integration needs a
  separate bounded object-frame queue.

## Proposed source layout

Keep the codec core independent of Qt, Windows, and the playback workers:

```text
src/codecs/eac3joc/
  bit_reader.h
  eac3_access_unit.h/.cpp
  emdf_parser.h/.cpp
  joc_parser.h/.cpp
  oamd_parser.h/.cpp
  qmf_bank.h/.cpp
  joc_reconstructor.h/.cpp
  decoder_pipeline.h/.cpp
  ts103420_tables.h/.cpp

tools/atmos-joc-probe/main.cpp
tests/test_eac3joc.h/.cpp
```

Create a small CMake library target such as `AudioPlayerEac3JocCore` only when
the first reusable module is introduced. Link the standalone probe and unit
tests to it. Link the production `AudioPlayer` target only after Gate 6.

## Data contracts

Use explicit frame-owned data. Do not expose pointers into `AVPacket`,
`AVFrame`, or FFmpeg private decoder state.

```cpp
struct Eac3AccessUnit {
    std::vector<std::uint8_t> bytes;
    std::int64_t pts;
    std::int64_t duration;
    unsigned sampleRate;
    unsigned audioBlocks;       // must total 6 per completed access unit
    unsigned expectedSamples;   // 1536 for the supported initial path
    bool discontinuity;
};

struct DecodedDownmixFrame {
    std::int64_t pts;
    unsigned sampleRate;
    unsigned samplesPerChannel;
    ChannelLayout layout;
    std::vector<std::vector<float>> planarChannels;
};

struct ObjectAudioFrame {
    std::uint64_t sequence;
    std::int64_t pts;
    unsigned sampleRate;
    unsigned samplesPerObject;
    std::vector<std::vector<float>> planarObjects;
    std::vector<ObjectMetadata> metadata;
    std::vector<float> lfeBypass;
    bool metadataValid;
    bool fallbackToCoreDownmix;
};
```

`ObjectMetadata` must preserve standard-space position, gain, object/bed type,
active state, and the effective sample interval. Windows coordinate mapping is
the renderer adapter's responsibility.

## Required invariants

- Operate at the source sample rate. The initial supported path is 48 kHz.
- Preserve the FFmpeg decoder's native channel layout. Do not downmix or
  resample before JOC.
- Build complete E-AC-3 access units. One access unit represents six audio
  blocks and 1,536 PCM samples even when it contains several shorter
  syncframes.
- Parse the independent substream and every associated dependent substream.
- When dependent substreams are present, inspect the last dependent substream
  for the EMDF container carrying OAMD and JOC.
- Treat EMDF payload ID 11 as OAMD and payload ID 14 as JOC for this profile.
- Exclude LFE from the JOC matrix and carry it as a bypass signal.
- At 1,536 PCM samples and 64 samples per QMF analysis step, process 24 QMF
  time slots per completed access unit.
- Maintain a 640-sample QMF analysis state per input channel and the required
  synthesis state per reconstructed object across frame boundaries.
- Support at most 16 reconstructed OBA essences. Reject impossible counts and
  malformed table traversal without buffer overrun.
- Reset access-unit, QMF, JOC interpolation, and OAMD history on seek, splice,
  decoder flush, stream change, or sequence discontinuity.
- A missing or invalid metadata frame must produce an explicit core-downmix
  fallback decision. Do not reuse stale object positions or matrices.
- Never report renderer or audible success from decoder-only evidence.

### JOC downmix configuration policy

Treat `joc_dmx_config_idx` as a channel-identity contract, not as a request to
perform another stereo downmix or phase filter after E-AC-3 decoding:

| Config | JOC input order | Local validation policy |
| --- | --- | --- |
| 0 | L, R, C, Ls, Rs | structurally supported with synthetic coverage; no real sample |
| 1 | L, R, C, Ls, Rs, Lb, Rb | structurally supported with synthetic coverage; no real sample |
| 2 | L, R, C, Ls, Rs, Tfl, Tfr | structurally supported with synthetic coverage; no real sample |
| 3 | L, R, C, Ls, Rs | primary real-sample path (`media/03. iPad.m4a`) |
| 4 | L, R, C, Ls, Rs, Tfl, Tfr | secondary real-sample path (`media/POWDER SNOW Live V9.8.6.eb3`) |
| 5-7 | reserved | reject explicitly |

Configurations 3 and 4 indicate that a 90-degree phase shift was applied to
Ls and Rs before AC-3/E-AC-3 encoding. TS 103 420 still feeds the decoded
channel QMF directly to the same Pseudocode 7 reconstruction operation and
does not specify an inverse phase filter in the JOC decoder. Preserve the
native decoded samples and do not apply a second or inverse phase transform in
the JOC layer. Configurations 0/3 share the same five JOC channel identities;
configurations 2/4 share the same seven `5.X + 2` identities. Configuration 1
is the distinct seven-channel Lb/Rb case.

## Implementation gates

Each gate should be a separate reviewable commit. Stop on a failing gate rather
than continuing with compensating assumptions.

### Gate 0: Freeze the baseline

Goal: record the exact inputs and current FFmpeg behavior before implementation.

- Confirm `media/03. iPad.m4a` is decoded by the bundled libav decoder as
  48 kHz, six-channel float PCM.
- Record packet PTS, packet size, decoded `AVFrame::nb_samples`, channel layout,
  codec profile, and the number of syncframes contained in each of the first
  100 audio packets.
- Run the existing raw-stream topology probe on the `.eb3` sample.
- Do not change production playback code.

Acceptance:

- A deterministic probe report identifies whether one packet is already one
  complete 1,536-sample access unit.
- The report distinguishes syncframes, access units, independent substreams,
  and dependent substreams.

### Gate 1: Access-unit assembler

Goal: turn packet bytes or raw elementary-stream bytes into bounded complete
access units.

- Extend the current frame header with absolute bit limits, block count,
  frame type, substream ID, and byte-order handling.
- Group syncframes until every represented substream contributes six audio
  blocks for the same presentation interval.
- Preserve the original compressed bytes for later EMDF parsing.
- Reject truncation, reserved stream types, invalid frame sizes, unsupported
  sample-rate changes, and block-count mismatches.

Acceptance:

- The M4A and raw EB3 samples produce stable counts across repeated runs.
- Every completed initial-path access unit reports 48 kHz and 1,536 samples.
- Malformed and truncated unit tests fail without reading outside the input.

### Gate 2: EMDF extraction and synchronization

Goal: expose raw OAMD and JOC payloads without decoding their contents.

- Implement the TS 102 366 Annex H EMDF container parser.
- Walk all dependent substreams and locate the final applicable EMDF
  container.
- Extract payload ID, payload size, protection/configuration fields, and raw
  payload bytes for IDs 11 and 14.
- Record packet/access-unit PTS and the JOC sequence count when it becomes
  available.
- Keep unknown payloads bounded and skippable.

Acceptance:

- The Apple Music sample yields frame-aligned OAMD and JOC payloads, or the
  gate reports the exact syntax location where extraction fails.
- Payload bounds never exceed the enclosing syncframe or EMDF container.
- Sequence gaps, duplicates, and splice resets are reported explicitly.

### Gate 3: Native FFmpeg PCM pairing

Goal: pair each compressed access unit with unmodified decoder PCM.

- Feed the original packet to libavcodec and the same bytes to the sidecar
  parser.
- Capture planar PCM before `SwrContext` conversion.
- Require native 48 kHz and an eligible 5.x or 7.x channel downmix for the
  initial implementation.
- Map FFmpeg channel identities to the JOC input order defined by
  `joc_dmx_config_idx`; exclude and retain LFE separately.
- Verify the actual handling of all dependent substreams. Do not infer it from
  the Atmos codec profile alone.

Acceptance:

- One completed metadata access unit pairs with exactly 1,536 PCM samples per
  channel.
- PTS and cumulative sample position remain aligned for at least 30 seconds.
- No resampling or channel downmix appears in the probe path.
- If FFmpeg omits required dependent-substream audio, stop and document the
  missing layout before considering an FFmpeg patch.

### Gate 4: QMF perfect-reconstruction path

Goal: implement TS 103 420 complex QMF analysis and synthesis independently of
JOC.

- Use 64 subbands and the 640-entry prototype-filter coefficients.
- Implement streaming analysis and synthesis with persistent state.
- Test impulse, silence, sine sweeps, random finite signals, block boundaries,
  and state reset.
- Determine and report the exact algorithmic delay; align before comparing
  input and output.

Acceptance:

- No NaN, infinity, denormal storm, out-of-bounds access, or clipping is
  introduced by unity analysis/synthesis.
- Use the matrix `N` equation printed above TS 103 420 Pseudocode 14 for
  synthesis. Its expanded phase is `(2*j - 4*n + 1)`; Pseudocode 14's
  `(2*j - 2*n - 1)` contradicts that equation. ETSI TS 103 190-1 Pseudocode 66
  independently uses the matrix-equation phase for the same 64-band/640-tap
  QWIN table.
- After delay alignment, the initial float implementation reaches at least
  72 dB reconstruction SNR on every deterministic fixture, with a 577-sample
  delay and unity gain within 0.001. The floor is pinned just below the
  measured 73.10 dB impulse response of the official coefficient table;
  sine, sweep, and deterministic random fixtures measure approximately
  83.61, 78.24, and 78.30 dB respectively.
- Splitting the same input across arbitrary call boundaries yields the same
  aligned output as a single call. Resetting both filter states produces the
  same output as a fresh instance, silence remains exact zero, and the fixture
  run contains no subnormal samples.

### Gate 5: JOC side information and matrix reconstruction

Goal: reconstruct object QMF from channel QMF without OAMD rendering.

- Parse `joc_header`, `joc_info`, data-point information, sparse/non-sparse
  data, and reserved extension handling.
- Implement bounded Huffman traversal using the normative coarse/fine generic,
  sparse coefficient, and 5/7-channel index tables.
- Implement differential decoding, dequantization, parameter-band mapping,
  smooth/steep temporal interpolation, sequence/splice reset, and the complex
  reconstruction matrix.
- Implement channel-identity mapping for configurations 0 through 4, but make
  configurations 3 and 4 the required real-sample paths. Configurations 0, 1,
  and 2 may be reported as implemented-with-synthetic-coverage until a real
  sample is available; their absence must not block Gate 5C.
- Feed configuration 3 from native FFmpeg `5.1(side)` PCM and configuration 4
  from native FFmpeg `5.1.2` PCM. Select channels by explicit identities, not
  assumed planar indices, and bypass LFE.
- Do not add inverse 90-degree phase processing for configurations 3 and 4.

Acceptance:

- Synthetic table-vector tests cover every Huffman table and both quantizers.
- Known zero/unity matrix fixtures produce the expected silent/copied objects.
- Both real samples produce bounded finite object QMF with a stable 15-object
  count and continuous sequence state for at least 1,000 access units.
- Unsupported configurations fall back to core PCM; they do not silently use
  the wrong matrix.

### Gate 6: OAMD and renderer-neutral object frames

Goal: pair reconstructed object PCM with timed object metadata.

- Parse the OAMD payload sections required for object identity, bed/static
  assignment, active state, gain, position, and updates over the frame.
- QMF-synthesize every active reconstructed object to planar float PCM.
- Preserve OAMD coordinates and timing without Windows-specific conversion.
- Produce `ObjectAudioFrame` batches through a bounded diagnostic queue or
  callback.
- Define deterministic core-downmix fallback for absent, malformed, or
  unsupported OAMD/JOC.

Acceptance:

- Object PCM, object metadata, LFE bypass, PTS, and sample counts agree for at
  least 30 seconds of the Apple Music sample.
- Seek and flush restart all histories without carrying a pre-seek matrix or
  object position into post-seek output.
- A diagnostic export can write per-object WAV files plus a compact metadata
  timeline outside the repository; generated media is not committed.
- The output contract contains everything needed by a later Windows dynamic
  spatial-object adapter and contains no Windows API dependency.

### Gate 7: Production integration readiness review

Goal: decide whether the decoder is safe to connect to playback. This gate does
not implement the Windows renderer.

- Review CPU and memory cost for 16 objects at 48 kHz.
- Review backpressure, queue bounds, teardown, seek generation, and stale-frame
  rejection against the existing dual-worker model.
- Confirm the existing ordinary PCM path remains unchanged for non-JOC media.
- Decide whether FFmpeg public decoding is sufficient. Patch FFmpeg only when
  Gate 3 contains reproducible evidence of missing required channel audio.
- Define the later renderer adapter contract for dynamic-object capacity,
  OAMD-to-Windows coordinate mapping, object activation, and fallback to a
  static bed or stereo.

Acceptance:

- A focused design review has no unresolved ownership or lifetime ambiguity.
- Unit tests and the standalone probe pass from a clean build.
- Status documentation states decoder-layer evidence only; no headphone
  rendering claim is made.

## FFmpeg patch fallback

An FFmpeg source modification is a contingency, not the initial design. Enter
this path only if Gate 3 proves that public libavcodec output omits channel
audio required by TS 103 420.

If needed:

- reproduce the missing dependent-substream/channel case in a standalone
  decoder test;
- keep the patch narrow to dependent-substream assembly or decoded-channel
  exposure;
- do not put JOC, QMF, or OAMD rendering into FFmpeg;
- rebuild the project's slim static FFmpeg libraries from the pinned source;
- document the source commit, patch, build profile, and LGPL source-offer
  implications;
- preserve an unpatched FFmpeg A/B build until PCM equivalence is established.

Do not use FFmpeg's Atmos profile flag as proof that JOC or OAMD was decoded.

## Validation matrix

| Layer | Required evidence | What it does not prove |
| --- | --- | --- |
| Container/demux | packet bytes, PTS, duration, stream index | valid E-AC-3 syntax |
| Access-unit parser | syncframe/substream/block report | PCM correctness |
| FFmpeg core | native planar PCM, layout, sample counts | JOC/OAMD preservation |
| EMDF | bounded payload 11/14 extraction and sequence | object PCM correctness |
| QMF | aligned reconstruction metrics | JOC matrix correctness |
| JOC | deterministic matrix/object QMF tests | spatial position correctness |
| OAMD output | timed object PCM and metadata frames | Windows/Dolby rendering |
| Later endpoint test | Windows spatial submission and loopback/listening | decoder internals by itself |

Run the smallest relevant target during each gate. Before any production
integration commit, run:

```powershell
scripts\run-tests.ps1
```

Use a task-specific build directory if another agent is active. Generated
WAV, raw, JSON, logs, and reports stay under the build directory and out of
git.

## Current execution state

Gates 0 through 5C and Gates 6A through 6C are complete and locally
validated as standalone diagnostics. Gate 5C reconstructs
finite complex QMF for all 15 objects across 1,000 continuous access units of
both the Apple config-3 and DME config-4 samples, with explicit channel
identity mapping, LFE bypass, deferred container trim, and zero fallback. The
Gate 6C now emits a renderer-neutral, transactionally delivered timeline of 15
planar object streams, delayed LFE, and sample-aligned OAMD after common trim
and the 577-sample QMF tail flush for both samples.
The scoped Gate 5C prompt in `docs/dev/eac3-joc-gate5-luna-handoff.md` is
retained as an execution record and must not be treated as an unfinished task.
Gate 6 is split into separately reviewed synthesis, OAMD, and alignment phases
in `docs/dev/eac3-joc-gate6-luna-handoff.md`; all three phases have passed.
Gate 7 is split into a synthetic Windows dynamic sink, an explicit OAMD
property/geometry adapter, and only then a bounded decoder-to-renderer bridge;
Gates 7A and 7B have passed. Gate 7B transactionally converts all 15 objects to
finite listener-relative Windows positions and headroom-safe volumes with
sample-position ramp state. The active next boundary is Gate 7C's bounded
decoder-to-renderer bridge. The implementation and validation contracts are in
`docs/dev/eac3-joc-gate7-luna-handoff.md` and
`docs/dev/eac3-joc-gate7b-luna-handoff.md`. Gate 7C0/7C1 sparse-property,
bounded-queue, and render-quantum scheduling have passed; the active Windows
consumer slice is in `docs/dev/eac3-joc-gate7c-luna-handoff.md`.
No Media Foundation, Windows Spatial Audio, or Dolby Atmos for Headphones
listening result is implied by the completed decoder-side gates.
