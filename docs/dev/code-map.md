# Code map

Use this page to choose the first files to inspect. It is a navigation aid, not
a replacement for the current status trackers in `docs/bug/`.

## Playback backends

- WASAPI high-level player control:
  `src/backends/wasapi/windowswasapiaudioplayer.cpp`
- WASAPI render worker and submitted-PCM diagnostics:
  `src/backends/wasapi/windowswasapiaudioplayer_worker.h`
- WASAPI output device selection, active-switch transaction entry, hot
  reconfigure, and format negotiation:
  `src/backends/wasapi/windowswasapiaudioplayer_output.cpp`
- WASAPI state changes, recovery, decoder data handling, and anomaly tracking:
  `src/backends/wasapi/windowswasapiaudioplayer_state.cpp`
- WASAPI shared declarations:
  `src/backends/wasapi/windowswasapiaudioplayer.h`
- ASIO backend:
  `src/backends/asio/windowsasioaudioplayer.cpp`
- FFmpeg CLI backend:
  `src/backends/ffmpeg/ffmpegaudioplayer.cpp`
- In-process libav seek decoder:
  `src/backends/ffmpeg/libavseekdecoderworker.cpp`

## E-AC-3 JOC Decoder

- Gated implementation and Luna handoff plan through renderer-neutral object
  PCM/OAMD output:
  `docs/dev/eac3-joc-decoder-plan.md`
- Gate 7 Windows dynamic-object renderer plan and validation contract:
  `docs/dev/eac3-joc-gate7-luna-handoff.md`
- Gate 7B OAMD-to-Windows property, geometry, gain, and ramp adapter record:
  `docs/dev/eac3-joc-gate7b-luna-handoff.md`
- Gate 7C bounded decoder-to-renderer queue and scheduler plan:
  `docs/dev/eac3-joc-gate7c-luna-handoff.md`
- Production integration boundary, FFmpeg/libav reuse, and Gate 8 slices:
  `docs/dev/eac3-joc-production-playback.md`
- Full-chain architecture, standards dependency map, self-rendered headphone
  route, optional Windows comparison bridge, and Gate 8A-8D acceptance plan:
  `docs/dev/eac3-joc-full-chain-plan.md`
- Active dependency order from real-sample native feature inventory through
  decoder, scene, SOFA/BRIR, stereo WASAPI, and native M4A integration, with
  one-slice Luna handoff rules: `docs/dev/eac3-joc-next-roadmap.md`
- Gate 8B-1 scene contract and deterministic offline self-test:
  `tools/atmos-render/scene-model.h/.cpp` and
  `tools/atmos-render/scene-model-probe.cpp`
- Gate 8B-2 fixed-layout equal-power stereo diagnostic panner and gain ramp:
  `tools/atmos-render/stereo-diagnostic.h/.cpp` and
  `tools/atmos-render/stereo-diagnostic-probe.cpp`
- Gate 8C-1 fixed BS.2051 System H (`9+10+3`) layout contract and SOFA-order
  comparison probe:
  `tools/atmos-render/bs2051-layout.h/.cpp` and
  `tools/atmos-render/bs2051-layout-probe.cpp`
- Gate 8C-2 single-triplet BS.2127 §6.1.1 gain-solve primitive and bounded
  self-test:
  `tools/atmos-render/bs2127-triplet.h/.cpp` and
  `tools/atmos-render/bs2127-triplet-probe.cpp`
- Gate 8C-3a System H nominal convex-hull facet catalog and bounded
  first-valid Triplet/Quad selector (`VirtualHullFacet` remains a separate
  VirtualNgon region):
  `tools/atmos-render/bs2127-selector.h/.cpp` and
  `tools/atmos-render/bs2127-selector-probe.cpp`; the BS.2127 QuadRegion
  scalar primitive is in `tools/atmos-render/bs2127-quad.h/.cpp`
- Gate 8C-3b lower-pole VirtualNgon ring aggregation, virtual triangle
  first-valid, public Wdmx direct downmix, and final real-gain power
  normalization:
  `tools/atmos-render/bs2127-virtual-ngon.h/.cpp` and
  `tools/atmos-render/bs2127-virtual-ngon-probe.cpp`
- R0B deterministic BS.2127 first-region-valid dispatcher over the nominal
  System H identity, Triplet, Quad, and lower VirtualNgon regions:
  `tools/atmos-render/bs2127-point-source-panner.h/.cpp` and
  `tools/atmos-render/bs2127-point-source-panner-probe.cpp`
- R0C bounded BS.2127-1 §6.1.3 nominal-to-actual adapter for labelled real
  System H positions (nominal topology is preserved; real vectors feed the
  region solvers; generic layouts remain unsupported):
  `tools/atmos-render/bs2127-nominal-to-actual.h/.cpp` and
  `tools/atmos-render/bs2127-nominal-to-actual-probe.cpp`
- R0D configured System H 28-point nominal hull with the five direct-downmix
  lower virtual speakers and their real-position/one-to-one gain mapping:
  `tools/atmos-render/bs2127-system-h-configured-panner.h/.cpp` and
  `tools/atmos-render/bs2127-system-h-configured-panner-probe.cpp`
- R1A renderer-neutral scene adapter: explicit bed-label routing, object
  metadata interpolation/gap policy, generation checks, 22-channel R0D gain
  frames, and separate LFE gain:
  `tools/atmos-render/scene-adapter.h/.cpp` and
  `tools/atmos-render/scene-adapter-probe.cpp`
- R1B1 decoder-to-scene object-property contract: explicit coordinate
  representation/point-direction boundary, OAMD size presence and
  width/depth/height mapping, dB-to-linear gain conversion, and typed pending
  non-zero extent without extent math:
  `tools/atmos-render/scene-object-properties.h/.cpp` and
  `tools/atmos-render/scene-object-properties-probe.cpp`
- R1B2 explicit OAMD-room to listener-relative unit-direction conversion and
  deterministic renderer-neutral decoded target-group bridge for the scheduler:
  `tools/atmos-render/scene-object-properties.h/.cpp` and
  `tools/atmos-render/scene-object-bridge.h/.cpp`
- R1B3 decoded-target ramp scheduler probe with Cartesian interpolation and
  exact evaluated snapshots (single-group scheduler):
  `tools/atmos-render/scene-object-ramp-scheduler.h/.cpp`
  and `tools/atmos-render/scene-object-ramp-scheduler-probe.cpp`
- R1B4 causal Gate6C co-timed grouping and one-next-group feed seam:
  `tools/atmos-render/scene-object-stream-grouper.h/.cpp` and
  `tools/atmos-render/scene-object-stream-grouper-probe.cpp`
- R1B5 evaluated snapshot to renderer-neutral System H gain frame:
  `tools/atmos-render/scene-object-gain-frame.h/.cpp` and
  `tools/atmos-render/scene-object-gain-frame-probe.cpp`
- R2A1 local-only SOFA extractor and versioned native BRIR cache loader:
  `scripts/extract-system-h-brir.py` and
  `tools/atmos-render/sofa-brir-cache.h/.cpp` with
  `tools/atmos-render/sofa-brir-cache-probe.cpp`
- Local ignored standards-cache manifest with edition/date, official URL,
  SHA-256, purpose, and access/license notes:
  `docs/dev/eac3-joc-reference-manifest.md`
- Local-only Dolby Encoding Engine Atmos/5.1 coverage-vector hashes,
  parameters, syntax evidence, and CLI profile boundary:
  `docs/dev/eac3-dee-test-vectors.md`
- Local-only SOFA/BRIR renderer asset provenance, hashes, dimensions, and
  license boundary: `docs/dev/eac3-joc-renderer-assets.md`
- Current Luna implementation slice for bounded Gate 5A JOC syntax/Huffman
  parsing, with later Gate 5B/5C boundaries:
  `docs/dev/eac3-joc-gate5-luna-handoff.md`
- Gate 0 packet/PCM correlation diagnostic:
  `tools/atmos-joc-probe/gate0.cpp` (`Eac3Gate0Probe`)
- Gate 1 access-unit assembler, Gate 2 EMDF extraction, and Gate 3 native PCM
  pairing diagnostic:
  `tools/atmos-joc-probe/access-unit.cpp` (`Eac3AccessUnitProbe`)
- Current syncframe/topology diagnostic:
  `tools/atmos-joc-probe/main.cpp`
- Gate 8N-1a/1b native E-AC-3 bounded syntax probes, including strict
  normalization of the observed DEE 5.2.1 16-byte Blu-ray-profile EB3
  carriage wrapper (8N-1b stops at the audfrm boundary; no FFmpeg):
  `tools/atmos-joc-probe/native-eac3-core.{h,cpp}` and
  `tools/atmos-joc-probe/native-eac3-bsi.{h,cpp}`
- Gate 8N-4a ordered native substream inventory and bounded AU-topology probe
  (reuses core/BSI parsing and the shared transactional AU assembler; no
  renderer/mixer or FFmpeg dependency):
  `tools/atmos-joc-probe/native-eac3-substream-probe.cpp`
- Gate 8N-5a bounded native ordinary-core adapter and config-3 PCM contract
  (stable coded channel IDs/LFE, contiguous 1,536-sample AUs, separate EOS
  tail, fail-stopped reset/poison semantics; standalone layer under the
  additive N5B seam):
  `tools/atmos-joc-probe/native-eac3-core-decoder.{h,cpp}` and
  `tools/atmos-joc-probe/native-eac3-core-decoder-probe.cpp`
- Gate 8N-5b ordinary native-core callback-seam adapter and focused contract
  probe (explicit `OrdinaryEac3`/`CoreContentKind`, stable coded channel IDs
  and LFE, 1,536-sample AU plus separate 256-sample EOS tail, callback
  backpressure, external timestamp-base preservation, and fail-stopped reset;
  no JOC qualification or playback):
  `tools/atmos-joc-probe/native-eac3-core-decoder-seam.{h,cpp}` and
  `tools/atmos-joc-probe/native-eac3-core-decoder-seam-probe.cpp`
- Gate J0A1 native config-3 EMDF raw-payload extractor (strict `0x5838`
  container/protection/bounds parsing, raw payload 11/14 delivery, no JOC
  qualification, no PCM/renderer/FFmpeg):
  `tools/atmos-joc-probe/native-eac3-emdf.{h,cpp}` and
  `tools/atmos-joc-probe/native-eac3-emdf-probe.cpp`
- Gate J0A2 native fail-closed JOC/OAMD qualification boundary (reuses public
  Gate 5A and Gate 6B1 parsers; supplied config-3 payload14 qualifies when both
  payload parsers return Pass):
  `tools/atmos-joc-probe/native-eac3-joc-qualifier.{h,cpp}` and
  `tools/atmos-joc-probe/native-eac3-joc-qualifier-probe.cpp`
- Gate J0A3 native config-3 bridge (strict N5B/J0A2 AU alignment, coded-channel
  reorder, B1->B2A->B2B metadata, Gate6C session callback, separate EOS tail;
  no renderer/playback/config4/advanced syntax):
  `tools/atmos-joc-probe/native-eac3-joc-session-bridge.{h,cpp}` and
  `tools/atmos-joc-probe/native-eac3-joc-session-bridge-probe.cpp`
- Gate 6B2C normative OAMD additional-element parser and B2B effective-state
  seam: B1 ID-2 `trim_element` and ID-5 `extended_object_element` are parsed
  with bounded readers, TS 103 420 reserved/trailing-bit fail-closed checks,
  while unknown elements honor `b_discard_unknown_element` only after their
  bounded length is validated,
  divergence reuse/inactive handling, nine trim configurations, and extended
  position codewords.  B2B applies these atomically before committing state;
  it does not invent a renderer algorithm:
  `tools/atmos-joc-probe/oamd-additional.{h,cpp}`,
  `tools/atmos-joc-probe/oamd-additional-probe.cpp`, and
  `tools/atmos-joc-probe/oamd-b2b.{h,cpp}`
- Gate 8N-1b BSI LFE-mix metadata and Annex-H headphone signalling probes:
  `tools/atmos-joc-probe/native-eac3-bsi.{h,cpp,probe.cpp}` exposes the
  normative `lfemixlevcode`/`lfemixlevcod` fields, while
  `tools/atmos-joc-probe/annex-h-headphone.{h,cpp,probe.cpp}` validates the
  H.3.7 header, presence flags, 32-frame BRIR chunk reassembly, raw-24-bit
  contract, and parity; `access-unit.cpp` routes bounded ID-7 bytes with AU
  ordinal/timestamp/substream context into the per-stream cache. No headphone
  renderer is connected.
- Gate J0A4-J0A6 config-4 association, mapped seven-slot input, diagnostic
  native JOC session entry, and layer/programme audit telemetry (mapped PCM,
  interpolated matrix, reconstructed object QMF, synthesized object PCM):
  `tools/atmos-joc-probe/native-eac3-config4-joc-bridge-probe.cpp`,
  `native-eac3-config4-mapping-probe.cpp`, and
  `native-eac3-config4-joc-session-probe.cpp`
- Gate J0A7-J0A8 config-4 prepared-scene entry and max-AU/EOS ownership:
  `tools/atmos-render/native-eac3-config4-scene-probe.cpp` and
  `tools/atmos-render/native-eac3-config4-scene.h`. Directional updates remain
  strict unit vectors; OAMD updates use an explicit ETSI-room Cartesian scene
  type, including a real listener-centre point, through allocentric BS.2127
  evaluation.
- Gate 8N N0 bounded audfrm/audblk inventory (ordinary six-block cursor;
  deterministic 1000-AU config-3/config-4/DEE feature matrix, structured
  Unsupported boundary for advanced branches; no FFmpeg):
  `tools/atmos-joc-probe/native-eac3-audblk.{h,cpp}` and
  `tools/atmos-joc-probe/native-eac3-audblk-probe.cpp`
- Gate 8N-1d/N1A state-only frame/block/channel snapshots and deterministic
  state digest (no coefficient values, PCM, DRC, IMDCT, or playback):
  `tools/atmos-joc-probe/native-eac3-block-state.{h,cpp}` and
  `tools/atmos-joc-probe/native-eac3-block-state-probe.cpp`
- Gate 8N-2a native exponent strategy/decode primitives (no bit allocation,
  mantissa, coupling coordinates, or IMDCT):
  `tools/atmos-joc-probe/native-eac3-exponents.{h,cpp}`
- Gate 8N-2b-1 native parametric bit allocation for one uncoupled FBW channel
  (no mantissas, coupling, AHT, SPX, enhanced coupling, or GAQ):
  `tools/atmos-joc-probe/native-eac3-bit-allocation.{h,cpp}`
- Gate 8N-2b-3 native mantissa degroup/dequantization primitive (reference
  dither source plus explicit injection; non-owning byte view; no audblk or
  IMDCT):
  `tools/atmos-joc-probe/native-eac3-mantissas.{h,cpp}`
- Gate 8N-2b/N2A1 ordinary uncoupled renderer-neutral spectral coefficient
  state probe (shared audfrm/audblk cursor; N2A2 reference dither/session
  reset; no IMDCT/PCM/JOC/playback/DRC):
  `tools/atmos-joc-probe/native-eac3-coefficient-probe.cpp`, with coefficient
  mode exposed by `native-eac3-audblk.{h,cpp}` and vector digest support in
  `native-eac3-block-state.{h,cpp}`
- Gate R2C/R2C2 diagnostic System H speaker-bus mixer and Cartesian future-point
  evaluation:
  `tools/atmos-render/system-h-planar-mixer.{h,cpp}`,
  `native-eac3-config4-r2c-probe.cpp`, and
  `bs2127-system-h-cartesian-panner.{h,cpp}`
- Gate I0P probe-local offline output bridge (R2C 22-speaker batches through
  R2B1, float32 stereo WAV, separate float32 LFE stem, JSON evidence; no
  production playback): `tools/atmos-render/i0p-offline-output.{h,cpp}`, exposed
  through the explicit `--i0p-*` options of
  `native-eac3-config4-r2c-probe.cpp`

- I0 amplitude/program-completeness audit: `scripts/run-atmos-i0-amplitude-oracle.ps1`
  decodes an exact AU count with the project's self-built FFmpeg runtime,
  records per-channel scale/timeline evidence, and compares the native R2C
  observation without treating 5.1.2 as an object-rendering oracle.
- Decoder-to-renderer object-property coverage matrix:
  `docs/dev/eac3-joc-property-coverage.json`, checked by
  `scripts/test-atmos-property-coverage.ps1`. Status is recorded independently
  for decoder, scene, renderer, and current MONTERO sample presence.
- I0 single-artifact listening render:
  `scripts/render-atmos-i0-listening-preview.ps1` drives the native config-4
  or config-3 JOC -> 15-object -> BS.2127 System H -> BRIR path, supports a
  157-AU preview or automatic full-file AU count, validates the float32 stereo
  WAV, packet-copies a selected M4A E-AC-3 stream without transcoding, and
  writes an explicit non-additive programme-carrier policy report. If natural
  stereo exceeds unity it retains that evidence and writes a separately named,
  explicitly reported post-BRIR monitor-gain listening copy. `-Jobs 0` selects
  up to eight independent Cartesian sample-evaluation workers; `-Jobs 1`
  retains the serial performance baseline.
  Container input uses `Eac3AccessUnitProbe --dump-eac3` for selected-stream
  packet copy without transcoding; output remains under `tmp`.
- I0 offline performance/profile seam:
  `tools/atmos-render/native-eac3-config4-r2c-probe.cpp` reports decode,
  evaluator, mix, metrics, BRIR append, and total timings; completed speaker
  batches are consumed by metrics/BRIR immediately while prepared object
  batches remain retained for future-point evaluation.
- Gate 8N-3a standalone scalar transform reference (TS §6.9.4 long/short
  IMDCT, Table 6.33 window, 256-sample overlap-add, explicit reset/flush;
  corrected Step-5 long index and bounded packed-window alignment PASS;
  no audblk integration or production PCM session):
  `tools/atmos-joc-probe/native-eac3-transform.{h,cpp}` and
  `tools/atmos-joc-probe/native-eac3-transform-probe.cpp`
- Gate 8N-3b/N3B bounded ordinary-uncoupled diagnostic PCM session (single
  audblk parse, continuous adjacent syncframes with per-channel
  dither/transform state, reset only for new stream/seek/discontinuity/
  topology rebuild, and EOS; supports explicit diagnostic-only interleaved
  f32le export for offline comparison; no production PCM acceptance, playback,
  or renderer):
  `tools/atmos-joc-probe/native-eac3-pcm-session.{h,cpp}` and
  `tools/atmos-joc-probe/native-eac3-pcm-session-probe.cpp`
- Current Windows static spatial-bed diagnostic:
  `tools/spatial-bed-probe/main.cpp`

## Source Preparation

- Source probe and metadata:
  `src/core/playbacksourceservice_probe.cpp`
- Source preparation and cache/remux flow:
  `src/core/playbacksourceservice_prepare.cpp`
- Shared playback source service orchestration:
  `src/core/playbacksourceservice.cpp`

## UI And Factory

- Main window UI logic:
  `src/ui/mainwindow.cpp`
- Startup, CLI automation, JSON report writing, and log setup:
  `src/ui/main.cpp`
- Backend selection:
  `src/core/audioplayerfactory.cpp`

## Linux ALSA 后端

- ALSA 后端主类：
  `src/backends/alsa/linuxalsaaudioplayer.cpp`
- ALSA 输出线程：
  `src/backends/alsa/alsaoutputworker.cpp`
- ALSA 格式协商：
  `src/backends/alsa/alsaformatnegotiator.cpp`
- ALSA 设备枚举和格式选择：
  `src/backends/alsa/linuxalsaaudioplayer_output.cpp`
- ALSA 状态管理和错误恢复：
  `src/backends/alsa/linuxalsaaudioplayer_state.cpp`

## Harness And Diagnostics

- Structure split plan for oversized files and agent-sized task slices:
  `docs/dev/structure-split-plan.md`
- Next Claude Code handoff for remaining structure cleanup:
  `docs/dev/claude-next-structure-handoff.md`
- Single-case smoke runner:
  `scripts/run-playback-smoke.ps1`
- Shared smoke helper functions:
  `scripts/harness-common.ps1`
- Regression matrix runner:
  `scripts/run-playback-regression.ps1`
- Harness/report schema check:
  `scripts/test-harness-reports.ps1`
- Log artifact attribution:
  `scripts/analyze-audio-artifacts.ps1`
- Evidence bundle collection:
  `scripts/collect-playback-evidence.ps1`
- Loopback manual wrapper:
  `scripts/run-loopback-manual-smoke.ps1`
- Loopback capture tool:
  `tools/wasapi-loopback-capture/main.cpp`

## Status Trackers

- Tracker index:
  `docs/bug/README.md`
- WASAPI anomalies:
  `docs/bug/wasapi-anomaly-status.md`
- ASIO:
  `docs/bug/asio-status.md`
- Playback cache/source preparation:
  `docs/bug/playback-cache-status.md`
- Harness/report contracts:
  `docs/bug/harness-report-status.md`
- E-AC-3/JOC/Atmos native decoder and self-rendered headphone route:
  `docs/bug/eac3-joc-status.md`
- Media Foundation and raw Dolby sidecar investigation/history:
  `docs/bug/media-foundation-status.md`

## Atmos scene contract

- Decoder-neutral OAMD property carriage and explicit disposition policy:
  `tools/atmos-render/scene-object-properties.{h,cpp}`
- Cartesian/directional scene update validation and gain-frame emission:
  `tools/atmos-render/scene-adapter.{h,cpp}`
- Sample-accurate metadata ramp/hold snapshots:
  `tools/atmos-render/scene-object-ramp-scheduler.{h,cpp}`
- Machine-checked TS 103 420 property coverage:
  `docs/dev/eac3-joc-property-coverage.json`,
  `scripts/test-atmos-property-coverage.ps1`

The scene contract retains priority, extent, zone, snap, anchor, distance, and
unsupported-property state without assigning renderer-specific sound rules.
Unsupported geometry remains fail-closed; TS 102 366 Annex-H payload `0x7`
is not present in the current samples.
Speaker-anchor lookup and bounded snap/zone candidate selection live in
`scene-adapter.cpp`; decoder fields absent from the current path (warp/trim,
balance, and conditional LFE mix metadata) remain explicit `NO_MIX`/policy-open
edges rather than inferred behavior.

- R2B1 renderer-neutral CPU BRIR seam: `radix2-fft.*`,
  `brir-convolver.*`, and `brir-convolver-probe.cpp`. The fixed-block core
  uses P=1024/F=2048/K=16 by default; the offline stream wrapper owns frame
  buffering and tail cropping. It has no decoder, PCM sink, DRC, or playback
  dependency.
