# E-AC-3 JOC full-chain plan

This document is the implementation boundary for a self-rendered,
headphone-oriented E-AC-3/JOC player. It consolidates the decoder work, the
renderer-neutral scene contract, the binaural reference path, and the
optional Windows Spatial Audio comparison bridge. It is a plan and evidence
map; it does not claim that Gate 8 or formal binaural rendering has been
implemented.

The active, dependency-ordered implementation slices and Luna handoff boundary
are maintained in `docs/dev/eac3-joc-next-roadmap.md`. This document remains
the architecture, standards, and acceptance boundary.

## Scope and two target routes

The primary chain starts with an E-AC-3/JOC compressed stream and ends in
stereo PCM produced by the application's own headphone renderer:

```text
compressed E-AC-3
  -> access units + EMDF
  -> native E-AC-3 core PCM
  -> self JOC/QMF reconstruction + OAMD
  -> bed/object PCM + metadata
  -> SceneAdapter -> RenderScene + MetadataTimeline
  -> self BS2127 panning -> self BS2051 virtual speakers
  -> direct/diffuse split -> SOFA HRTF/BRIR
  -> fractional delay + partitioned convolution
  -> stereo float PCM -> ordinary WASAPI stereo sink
```

The primary self-rendered headphone route is the completion target. Its output
must use ordinary WASAPI stereo with system Spatial Sound disabled, so Windows
or a Dolby endpoint cannot spatialize the already-binaural signal a second
time. `ListenerPose` starts as identity; head tracking and multi-view behavior
are later work.

The optional Windows spatial comparison route can submit object PCM through
the public `ISpatialAudioClient`/`ISpatialAudioObjectRenderStream` contract
(object PCM, dynamic position, volume, and static identity) when the selected
endpoint exposes the required stream and capacity. Endpoint HRTF/room/reverb
is not universal, and the application cannot inspect or control a Dolby
private renderer. This route is non-normative, is not a completion condition,
and cannot stand in for the application's headphone renderer.

This is the only primary Gate 8 chain. A native E-AC-3 core (Gate 8N) is a
necessary condition for completing Gate 8; a libavcodec adapter cannot satisfy
the production `ICoreDecoder` boundary.

## Current production chain

The current player is still an ordinary PCM chain; the primary self-rendered
headphone route is not in production:

```text
WindowsWasapiAudioPlayer::startPipeline()
  -> spatialStaticBedEnabledForSource()
  -> LibavSeekDecoderWorker or FfmpegDecoderWorker
  -> libavcodec E-AC-3 AVFrame
  -> libswresample / optional DolbyDownmix
  -> PcmStreamBuffer
  -> WasapiOutputWorker
     -> ordinary IAudioClient
     or 8-channel static Spatial Audio bed
```

Source locations:

- `src/backends/wasapi/windowswasapiaudioplayer.cpp:942-1000` selects the
  decoder/output format. The current spatial request is only `eac3` plus eight
  source channels and forces 48 kHz, float32, 5.1.2.
- `src/backends/wasapi/windowswasapiaudioplayer_state.cpp:215-226` performs
  that codec/channel test; it does not inspect EMDF, OAMD, or JOC.
- `src/backends/ffmpeg/libavseekdecoderworker.cpp:319-423,600-895` owns the
  in-process demux/decode/SWR/PCM append path. Compressed packets are not
  retained for a production JOC sidecar.
- `src/backends/wasapi/windowswasapiaudioplayer_worker_spatial.cpp:71-267`
  activates eight fixed static objects with no dynamic objects.
- `src/backends/wasapi/windowswasapiaudioplayer_worker_spatial.cpp:398-467`
  copies interleaved PCM channels into those static objects; it does not apply
  OAMD position or gain.

There is no production reference to `Eac3AccessUnitProbe`, Gate 6C, or the
Gate 7C renderer in `src/`. The current production static bed therefore must
not be described as production JOC object rendering.

## Current diagnostic chain

The current native config-4 diagnostic chain is:

```text
N5J native probe-local base/dependent PCM owner
  -> J0A4 qualification + J0A5 config-4 mapping
  -> J0A6 JOC/QMF/OAMD session (15 objects + LFE)
  -> J0A7 prepared scene with retained room positions
  -> J0A8 157-AU/final-EOS ownership
  -> R2C2 sample-accurate Cartesian System H speaker bus
  -> I0P diagnostic PASS: R2A1/R2B1 BRIR bridge + stereo WAV/LFE stem/report
  -> I0 (next): external PCM amplitude oracle + evidence-bounded listening
```

The older `Eac3AccessUnitProbe` plus Gate 7C2 Windows dynamic-object/7D
loopback route remains a separate diagnostic comparison path. Neither route is
production integration, and the current Cavern versus Dolby binaural
comparison remains a renderer-quality comparison rather than proof of a
decoder defect.

## Responsibility split

| Layer | Responsibility | Explicit non-responsibility |
| --- | --- | --- |
| AudioPlayer self-written core | Native E-AC-3 decode, AU assembly, EMDF/OAMD/JOC parsing, QMF, matrix reconstruction, object/LFE frames, OAMD state, `RenderScene`, panning, binaural rendering, bounded bridge | Do not put JOC/OAMD into FFmpeg private decoder state or ordinary `PcmStreamBuffer` |
| FFmpeg/libavcodec | Current legacy player's ordinary E-AC-3 PCM path and development-only offline differential oracle | Not linked into Atmos runtime; cannot implement production `ICoreDecoder`, decode JOC/OAMD objects, or be an acceptance dependency; do not use the Atmos profile flag as proof of JOC |
| Primary self-renderer | Scene normalization, BS2127/EAR panning, virtual speaker layout, direct/diffuse treatment, SOFA/BRIR validation, delay/convolution, stereo PCM | Cannot claim Dolby private renderer equivalence or use unvalidated room assumptions |
| Windows Spatial Audio comparison bridge | When available: mono float object buffers, dynamic position/volume, static identity, stream/event lifecycle | No public generic Dolby BRIR/RT60/room/object-size/zone/snap submission contract; not the primary route |
| Dolby endpoint | Final processing of the selected endpoint, if its private renderer is available | Not application-owned, inspectable, or a documented custom metadata sink |

## Primary renderer architecture

The decoder output is normalized once into a renderer-neutral scene. The
primary module/API boundary is:

```text
SceneAdapter
  -> RenderScene
       -> RenderElement(Object | Bed | DirectSpeaker | HOA)
       -> MetadataTimeline
       -> ListenerPose (identity first)
       -> SpeakerLayout
       -> PointSourcePanner / ExtentPanner / GainInterpolator
       -> SofaDatabase -> BinauralRenderer -> WasapiStereoSink
```

`SceneAdapter` converts OAMD/JOC frames and the fixed bed channel labels into
`RenderScene`; it does not expose decoder-private structures to the audio
sink. `RenderScene` owns timestamped elements and `MetadataTimeline` owns
bounded property interpolation. `SpeakerLayout` is the BS2051 virtual
speaker layout used by BS2127/EAR panning. `PointSourcePanner` handles point
objects and direct speakers; `ExtentPanner` handles width/height/depth and
the direct/diffuse split. `GainInterpolator` makes interpolation and
`jumpPosition` behavior explicit. `SofaDatabase` validates and serves
SOFA/BRIR data, `BinauralRenderer` performs delay/convolution, and
`WasapiStereoSink` submits only stereo float PCM to ordinary WASAPI.

The real-time render thread must perform no allocation, file I/O, lock
waiting, or formatted logging. SOFA loading, scene allocation, layout
construction, and IR preparation happen before the render generation starts.
The offline and real-time paths share the same renderer core and reset
contracts; only the source/sink scheduler differs.

### Bed, layout, and acoustic policy

Bed channel labels map through one fixed, tested table to the project
`SpeakerLayout`; a positional guess based on array order is not permitted.
LFE treatment is an explicit project policy and a separately tested branch.
For the first offline I0 artifact only, use `ExcludedFromBinaural` while
preserving an independent LFE stem and level/accounting report; this is
observable exclusion, not silent loss. BEAR's reference practice removes LFE
from the binaural path, but the I0 choice is a reversible diagnostic policy,
not the final listening-quality decision. A redirect, attenuation, or final
mix must be separately reviewed; an arbitrary -15 dB attenuation must not be
called normative.

The BS2051 virtual-speaker layout must use source positions that match the
SOFA/BRIR measurement positions. Do not hard-code a 10 x 10 x 3.5 metre room
as an acoustic model; room dimensions may describe a test fixture, not the
renderer physics.

BS2127 support is staged. P1 starts with `position`, `gain`, `width`,
`height`, `depth`, `diffuse`, `jumpPosition`, and `interpolation`. P1.1 adds
`channelLock`, `zoneExclusion`, `objectDivergence`, `screenRef`, and
`importance`. HOA and multi-view handling remain P2 work. BS2127 supplies
the virtual-loudspeaker panning/reference renderer boundary; it does not
provide the SOFA HRTF/BRIR algorithm.

For each render quantum, direct/diffuse gains use the documented square-root
split, with decorrelation, delay alignment, and gain compensation measured as
separate stages. The order is object/bed-to-virtual-speaker mix first, then
one convolution path per virtual speaker to the two ears. A 48 kHz sample
rate and 128/256-sample early plus 512/1024-sample late partitions are
candidate starting points only; CPU load, WASAPI period, latency, and
underrun margin must be measured before selecting them.

`SofaDatabase` rejects a file unless its sample rate, units, coordinate
convention, source-position coverage, listener/receiver definitions, IR
length, and license are compatible. It distinguishes free-field HRTF from
room BRIR and records the selected source coverage. SOFA is a data format,
not a panning, decorrelation, delay, or convolution algorithm.

The renderer reset clears `MetadataTimeline`, gain interpolation,
fractional-delay state, decorrelation state, convolution overlap, BRIR
crossfade state, HOA/listener caches, and the output queue. A new generation
cannot consume any old state.

Media Foundation remains diagnostic-only. The current MFPlay path accepted an
E-AC-3 sidecar but produced zero endpoint loopback frames, while the tested
Media Engine path failed readiness. For the tested selected USB endpoint only,
no usable compressed DD+/JOC application format was reported; this is not a
claim about all Windows endpoints or Dolby devices. Keep FFmpeg PCM only for
the current legacy ordinary-player path and offline differential checks. It is
not part of the Atmos runtime or Gate 8 acceptance.

## E-AC-3 core decoder strategy

Gate 8A should introduce a narrow `ICoreDecoder` boundary between the JOC
session and the native base E-AC-3 decoder. The production implementation of
`ICoreDecoder` must be the self-written native core; a libavcodec adapter is not
allowed in the Atmos runtime or as a Gate 8 acceptance dependency.

The reviewed pinned/current-project FFmpeg `eac3dec.c` is retained only as an
offline development differential oracle. It records these limits for that
oracle, not for the production decoder strategy:

- Enhanced Coupling, Reduced Sample Rates, and Transient Pre-noise Processing
  (TPNP) remain unsupported in the reviewed decoder path;
- when `substreamid != 0`, Additional substreams are skipped rather than being
  exposed as a complete native decoder assembly.

When the pinned FFmpeg commit is updated, this capability review and the
standalone substream tests must be repeated before changing the support set.

These limits must be surfaced by the decoder capability/result contract. A
JOC candidate cannot silently claim support when the required dependent or
additional substream audio is unavailable. The current supplied config-3 and
config-4 samples remain eligible only because their actual native PCM and
substream behavior have been separately measured by the diagnostic pairing
gate.

The final native decoder route is **Gate 8N** and is a prerequisite for any
Gate 8 production integration. Gate 8A may develop the session boundary and
tests earlier, but it cannot be accepted as a production Atmos route until
Gate 8N supplies the native `ICoreDecoder`:

```text
Gate 8N-1  TS 102 366 header/block syntax and bounded syncframes
Gate 8N-2  exponents, bit allocation, mantissas and coupling
Gate 8N-3  SPX, AHT and IMDCT reconstruction
Gate 8N-4  dependent/additional-substream assembly and channel identity
Gate 8N-5  conformance vectors + offline FFmpeg + optional DEE differential validation
```

Gate 8N must compare at separate evidence layers: syntax fields and block
counts, decoded native channel PCM, channel layout/substream assembly, and
post-decoder JOC/QMF output. Use conformance vectors as the acceptance basis;
the reviewed FFmpeg version is only an offline differential oracle. Only when
separately licensed and reproducible, use DEE-generated bitstreams paired with
known source PCM as an optional differential reference; DEE is neither
normative nor a hard dependency. Gate 8N must complete before any Gate 8
production acceptance, preserving a runtime path with no FFmpeg dependency.

## Normative and reference dependency map

The two ETSI PDFs and `ts_103420_tables.c` already present under `docs/dev/`
are local, untracked reference material. Do not replace them, redistribute
them, or add them to Git as part of this work. For additional standards, use an
ignored local reference cache downloaded only from the official source; record
edition, publication date, SHA-256, and license/access terms in the local cache
manifest. Repository Markdown keeps the version, purpose, and official URL;
PDFs remain outside Git.

### Core codec and object audio

- **ETSI TS 102 366 V1.4.1** — E-AC-3 core syntax, syncframes, block counts,
  dependent substreams, and EMDF container syntax. Individual E-AC-3
  syncframes may contain 1, 2, 3, or 6 blocks.
- **ETSI TS 103 420 V1.2.1** — backwards-compatible object-audio carriage
  using Enhanced AC-3: OAMD/JOC syntax, 64-band QMF, 640-sample
  prototype/filter length, JOC matrix, and object metadata semantics. Annex B
  is informative material describing OAMD-to-ADM relationships; it does not
  turn the E-AC-3 JOC bitstream into an ADM production carriage format.
- `docs/dev/ts_103420_tables.c` — local supplied Huffman/QMF table material;
  it is not a license substitute for the standard.

Required implementation contracts include six blocks and 1536 samples per
**complete TS 103 420 JOC access-unit grouping**; this is not a claim that each
E-AC-3 syncframe is naturally six blocks. Also require EMDF payload 11 for OAMD
and 14 for JOC in this profile, 24 QMF slots per complete grouping, LFE bypass,
explicit channel identity mapping, sequence/reset handling, and core-downmix
fallback when metadata is absent or invalid. The QMF synthesis phase
discrepancy recorded in `docs/dev/eac3-joc-decoder-plan.md:309-313` requires
standards review before treating the chosen equation as a distribution-grade
codec claim.

### ADM, layout, and renderer background

These references provide definitions or cross-checks; they do not change the
optional Windows comparison-bridge API boundary or make it the primary
headphone renderer:

- **ITU-R BS.2076-3 (2025-02)** — ADM model and definitions used to cross-check
  object/bed concepts. Official: `https://www.itu.int/rec/R-REC-BS.2076-3-202502-I/en`.
- **ITU-R BS.2094-2 (2025-02)** — common ADM definitions, including the
  Binaural type definition; terminology/interoperability cross-check only.
  Optional local reference. Official:
  `https://www.itu.int/rec/R-REC-BS.2094-2-202502-I/en`.
- **ITU-R BS.2125-1 (2022-05)** — serial ADM representation background;
  terminology/interoperability cross-check only. Official:
  `https://www.itu.int/rec/R-REC-BS.2125-1-202205-I/en`.
- **ITU-R BS.2051-3 (2022-05)** — advanced sound system and target speaker
  layouts. Official:
  `https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.2051-3-202205-I!!PDF-E.pdf`.
- **ITU-R BS.2127-1 (2023-11)** — ADM-to-BS.2051 target loudspeaker-layouts
  reference renderer specification. `typeDefinition=Binaural` is unsupported
  for this plan; BS.2127-1 does not represent the Windows/Dolby HRTF, BRIR, or
  room renderer. Official:
  `https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.2127-1-202311-I!!PDF-E.pdf`.
- **ITU-R BS.2466-1 (2022-09)** — ADM Renderer use/evaluation and QA
  background; it is not a normative renderer/API contract. Optional local
  reference. Official:
  `https://www.itu.int/dms_pub/itu-r/opb/rep/R-REP-BS.2466-1-2022-PDF-E.pdf`.
- **ITU-R BS.2159-9 (2022-03)** — multichannel sound technology in home and
  broadcasting applications; background report, not an ADM production
  implementation specification. Link only; do not download as a core
  dependency. Official:
  `https://www.itu.int/dms_pub/itu-r/opb/rep/R-REP-BS.2159-9-2022-PDF-E.pdf`.
- **EBU Tech 3396 / BEAR (2023-03)** — self-renderer headphone reference and
  binaural ADM renderer guidance. Required local reference for the primary
  route. Official:
  `https://tech.ebu.ch/publications/tech3396`.
- **AES69/SOFA** — primary-route HRIR exchange/reference only; link without
  downloading in this repository: `https://www.sofaconventions.org/`.
- **SMPTE ST 2098** — background/cross-reference only; link without
  downloading: `https://www.smpte.org/standards/st2098`.

The download policy is therefore: download to an ignored local cache, with
manifested edition/date/hash/license, **BS.2076-3, BS.2051-3, BS.2127-1, and
EBU Tech 3396**; optionally cache **BS.2094-2 and BS.2466-1**. Keep only
official URLs in Markdown for **AES69/SOFA, SMPTE ST 2098, and BS.2159-9**.

### Optional Windows comparison API boundary

The Gate 7 API contract in
`docs/dev/eac3-joc-gate7-luna-handoff.md:59-88` is the operative Windows
contract when the endpoint exposes the stream: query dynamic capacity, activate
mono float objects, use dynamic position/volume inside Begin/End, call GetBuffer
for every live object, and pair every successful Begin with one End. Windows
coordinates are listener-relative right-handed metres (+X right, +Y up, +Z
behind). Endpoint HRTF/room/reverb availability is endpoint-specific.

`ISpatialAudioObjectForHrtf` is explicitly a Windows Sonic for Headphones
route. It is not a way to enrich or control Dolby Atmos for Headphones.

## Fixed JOC differential-oracle policy

For the development-only FFmpeg differential oracle, `drc_scale=0` is a fixed
policy that only disables FFmpeg stream DRC scaling. It is not a normative
“full-range” decoder claim, a user volume control, or a production Atmos
runtime setting. Keep `heavy_compr=0`, `target_level=0`, do not request a
`downmix`, and preserve the decoder's native channel layout for the offline
pairing path. The native production core must define and test its own explicit
DRC policy without linking FFmpeg.

`cons_noisegen=0` is the deterministic setting for the offline FFmpeg oracle.
It controls only the seed and reproducibility of FFmpeg zero-mantissa
dither/noise; it is not spatial, room, HRTF, or renderer control. Gate 8N may
use a deterministic native test setting to make comparisons repeatable, but
this is not a listening A/B requirement and must not be presented as one.

## Open implementation and license boundary

The implementation is rewritten from public ETSI/ITU-R/EBU/SOFA-compatible
contracts and data descriptions. EAR/libear and BEAR may be used only as
version-pinned oracle/reference implementations; before integration, inspect
and record their LICENSE and NOTICE obligations. SOFA data files have their
own independent data licenses and must be approved per file. Do not copy
Dolby private HRTF/BRIR data, distance models, or tuning. The project may
claim open ADM/BS2127/BEAR-compatible behavior only for the tested public
subset. It must not claim to be a Dolby Atmos for Headphones clone or to match
Dolby listening results.

## Gate 8A-8D plan: primary self-rendered headphone route

The active execution boundary is recorded in
`docs/dev/eac3-joc-next-roadmap.md`. The historical Windows-only plan in
`docs/dev/eac3-joc-production-playback.md` is now explicitly Gate 8W: an
optional comparison bridge that cannot satisfy the primary route's acceptance.

The active implementation order is I0 after the accepted diagnostic I0P
(offline raw EB3), I1 (ordinary
stereo WASAPI), I2 (native M4A/trim/seek), then I3 (production/release). The
Gate names below are production acceptance groupings, not claims that their
future targets exist. Current N5/J0/R0-R2 components are bounded probe-local
primitives; they do not complete Gate 8N, 8A, 8B, 8C, or 8D.

### Gate 8A — reusable decoder session

Extract the real native AU/EMDF/OAMD/JOC/QMF/Gate 6C orchestration into a reusable
non-Qt or narrowly wrapped runtime session. It must support candidate result
`Joc`, `NotJoc`, `Unsupported`, `Malformed`; bounded callback backpressure;
cancellation; exact trim/flush accounting; generation reset; and the existing
diagnostics without per-AU console spam. The probe and production must use the
same session and the native `ICoreDecoder`. A separate offline FFmpeg probe may
remain for differential comparison, but it cannot be the session's production
decoder or label an AVFrame a self-written native decoder.

Existing legacy FFmpeg baseline commands (already available; they do not prove
Gate 8A or native Gate 8 completion):

```powershell
cmake --build build-gate8a --target Eac3AccessUnitProbe --config Release -- /m:1
.\build-gate8a\Release\Eac3AccessUnitProbe.exe "media\03. iPad.m4a" --max-units 1000 --joc-gate6c
.\build-gate8a\Release\Eac3AccessUnitProbe.exe "media\POWDER SNOW Live V9.8.6.eb3" --max-units 1000 --joc-gate6c
scripts\run-tests.ps1 -BuildDir build-gate8a -Configuration Release -NoBuild
```

Acceptance adds native-core/session self-tests, native PCM and substream
identity checks, and requires the existing 1000-unit object/metadata/flush
counts to remain equivalent on both samples. The offline FFmpeg oracle may be
used for differential evidence only and is not acceptance-critical. No target
named `Eac3ProductionJocSessionProbe` exists yet; do not report it as run.

### Gate 8B — promote the accepted offline scene/renderer to a stereo runtime

The standalone scene, metadata, System H, SOFA-cache, convolver, and R2C mixer
primitives already exist. I0P now integrates them offline and proves stereo
WAV, LFE-stem, reset/tail, level, and frame-accounting contracts. I0 must next
resolve the external PCM amplitude oracle and bounded listening evidence. Gate
8B then promotes that accepted renderer core behind an ordinary
`WasapiStereoSink`; it does not recreate a second panner. System Spatial Sound
remains disabled and ordinary PCM, ASIO, and ALSA behavior remain unchanged.

The future target and probe below are not present today:

```powershell
cmake --build build-gate8b --target Eac3SelfRendererProbe --config Release -- /m:1
.\build-gate8b\Release\Eac3SelfRendererProbe.exe "media\03. iPad.m4a" --renderer stereo-diagnostic --max-units 1000
scripts\validate-all.ps1 -BuildDir build-gate8b -Configuration Release -SkipSmoke
```

Gate 8B acceptance requires the accepted I0 core plus deterministic scene
snapshots, fixed bed labels, object identity/timestamps, play/pause/stop/EOS,
seek/reset with no old tail, finite PCM, no allocation or lock wait on the
render thread, and exact sink frame accounting. The commands cannot be run or
claimed until the target is added.

### Gate 8C — P1 BS2127/EAR virtual-speaker rendering

Add `Bed`, `DirectSpeaker`, and `Object` elements; BS2127
`position/gain/width/height/depth/diffuse/jumpPosition/interpolation`; the
P1.1 fields `channelLock/zoneExclusion/objectDivergence/screenRef/importance`;
fixed BS2051 virtual speaker layouts; and EAR/libear gain-oracle comparison.
The first implementation mixes objects/bed into virtual speakers, applies the
documented direct/diffuse square-root split, and makes jump/interpolation and
gain compensation observable. It still uses the stereo diagnostic sink until
P2.0 supplies measured HRTF/BRIR convolution.

```powershell
cmake --build build-gate8c --target Eac3SelfRendererProbe --config Release -- /m:1
.\build-gate8c\Release\Eac3SelfRendererProbe.exe "media\POWDER SNOW Live V9.8.6.eb3" --renderer virtual-speaker --oracle ear-libear
scripts\validate-all.ps1 -BuildDir build-gate8c -Configuration Release -SkipSmoke
```

These are proposed future commands, not existing validated commands. Gate 8C
acceptance includes single-object position/gain/extent/interpolation/jump
vectors, every fixed bed speaker label and explicit LFE policy, each virtual
speaker's impulse/IR mix, EAR/libear gain-vector tolerances, and no NaN,
clipping, stale generation, or unbounded latency.

### Gate 8D — P2 formal binaural renderer, recovery, and listening evidence

P2.0 is the first milestone that may be called formal headphone
spatialization: static SOFA/BRIR direct path, fixed listener, fractional
delay, early/late partitioned convolution, and `BinauralRenderer`. P2.1 adds
diffuse decorrelation, gain compensation, delay alignment, and BRIR crossfade.
P2.2 adds HOA, multi-view, listener pose, and head tracking; none is required
to call P2.0 complete. The object/bed-to-virtual-speaker mix precedes one
virtual-speaker-to-two-ear convolution path per speaker.

```powershell
cmake --build build-gate8d --target Eac3BinauralRendererProbe --config Release -- /m:1
.\build-gate8d\Release\Eac3BinauralRendererProbe.exe "media\03. iPad.m4a" --renderer sofa-static --listener identity
scripts\run-playback-regression.ps1 -BuildDir build-gate8d -CaseFilter @('*joc-seek*','*joc-reset*','*joc-binaural*') -NoCleanup
scripts\validate-all.ps1 -BuildDir build-gate8d -Configuration Release -SkipSmoke
```

All commands above are future targets/cases and must be reported as
unavailable until implemented. Gate 8D acceptance requires impulse sweeps
for one object at each tested azimuth/elevation/distance; each bed speaker
label and LFE behavior; extent/diffuse; jump/interpolation; one measured IR
for each virtual speaker; reset with no old convolution tail; gain vectors
against EAR/libear; power/NaN/clipping/latency and real-time budget; and
subjective checks for left/right, front/back, height, externalization,
coloration, and multiple objects. Numeric agreement is not Dolby listening
equivalence. Seek/reset must clear timeline, gain interpolation,
fractional-delay, decorrelation, convolution overlap, BRIR crossfade, HOA and
listener caches, and output queue before the new generation starts.

### Optional Gate 8W — Windows spatial comparison bridge

Only after the self-renderer has a stereo diagnostic or formal output may a
separate comparison bridge submit object PCM to a selected Windows endpoint.
Its proposed target is `Eac3WindowsSpatialComparisonProbe`; no such target or
validated command exists. It may compare endpoint loopback and timing, but
endpoint availability, private Dolby processing, and subjective Dolby sound
are outside the acceptance contract and cannot block or complete Gate 8A-8D.

## Fallback and stop conditions

Fallback is allowed only before object playback starts:

- no bounded EMDF/JOC/OAMD evidence;
- unsupported config, sample rate, channel identity, object count, or OAMD
  property;
- malformed/truncated AU, EMDF, Huffman, QMF, or metadata state;
- the native E-AC-3 core does not provide the required channel layout;
- primary renderer lacks a validated `SpeakerLayout`, compatible SOFA/BRIR,
  or a supported object/bed element;
- ordinary stereo WASAPI sink cannot be opened.

These cases must return a structured reason and use the unchanged ordinary PCM
path (the current legacy FFmpeg/libav player path) before self-rendered playback
starts. This fallback is not part of the Atmos runtime and cannot satisfy Gate 8
acceptance. The optional Windows comparison
bridge may independently report endpoint format/capacity failure and remain
disabled; that is not a primary-route fallback or a product failure. After
renderer playback starts, a deterministic decoder/renderer failure must be
reported and the generation stopped; do not silently switch render models
mid-track.

Stop the gate immediately on any of the following:

- a failing syntax, QMF, JOC, OAMD, timing, identity, or finite-value check;
- stale generation data, queue gap/overlap, missing End, or teardown ownership
  violation;
- metadata that requires unsupported size/zone/snap/distance behavior;
- a reproducible native-core missing-channel case not yet isolated in a
  standalone test;
- any attempt to claim Dolby headphone, HRTF, or audible success from decoder
  or endpoint-submission evidence alone.

## Current sample coverage and limits

Validated diagnostic samples are:

- `media\03. iPad.m4a`: E-AC-3/JOC config 3, 48 kHz, six-channel source,
  container trim/priming behavior.
- `media\POWDER SNOW Live V9.8.6.eb3`: E-AC-3/JOC config 4, 48 kHz, native
  5.1.2/eight-channel source, dependent-substream path.

The existing Gate 5C/6C/7B/7C diagnostics cover these supplied samples and
15 reconstructed objects. Configurations 0-2 have synthetic/structural
coverage only. No production Gate 8 target, JOC player regression case, seek
preroll rule, or fault-injection recovery target exists yet. The two samples
also lack Annex-H headphone payload `0x7`, so its absence is not evidence that
the current Dolby/Cavern listening difference is caused by discarded headphone
metadata.

The current evidence proves, at most, decoder-side object reconstruction,
Windows object submission, and (where separately captured) non-silent endpoint
PCM. It does not certify the primary self-rendered headphone route, SOFA/BRIR
coverage, formal binaural placement, renderer transparency, or pop-free
audible output. No current sample covers a licensed production SOFA set,
measured room BRIR, all BS2127 P1.1 fields, HOA, listener pose, or a complete
subjective multi-object headphone study.
