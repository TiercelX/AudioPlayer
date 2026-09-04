# Media Foundation Dolby status

This file tracks the Windows Media Foundation investigation for Dolby streams
that first pass through the existing raw-stream-to-Matroska sidecar flow. It
does not replace the FFmpeg decoder, WASAPI, or ASIO status trackers.

Current E-AC-3/JOC/Atmos implementation status is maintained in
`docs/bug/eac3-joc-status.md`. The entries below retain the Media Foundation /
sidecar investigation and historical cross-domain evidence; they are not the
authoritative current implementation order.

## Status refresh: 2026-08-27 (Renderer R1B1 decoder-to-scene contract; bounded PASS)

- **Contract**: `Eac3SceneObjectPropertiesProbe` now adapts Gate6C
  `MetadataUpdate`/`B2bObjectState` into a renderer-neutral property snapshot.
  Coordinate representation and the renderer point-direction conversion are
  explicit; invalid or unknown representations are rejected. Cartesian is
  never inferred from B2b's normalized coordinates.
- **Extent/gain**: `sizePresent` and `sizeIndex` are retained, including the
  `{width, depth, height}` to named `{width, height, depth}` reorder. Absent
  size, explicit zero, and non-zero size are distinct. Non-zero size is typed
  `EXTENT_PENDING` and cannot be silently passed to the point-only adapter.
  B2b now also retains effective size presence/index across render reuse or
  mixed blocks; `raw` remains the current B2a codeword and is not used as a
  substitute for the effective extent provenance.
  dB gain maps to linear gain, including flagged IEEE `-inf` to zero; invalid
  positive infinity and non-finite finite-gain inputs fail closed.
- **Timing/unsupported fields**: Gate6C source timestamp, block index, and
  ramp duration are preserved. An independently supplied jump flag is retained;
  missing jump, diffuse, and divergence remain structured unsupported fields.
- **Evidence**: The synthetic probe covers point/non-zero-size/presence/order,
  gain and `-inf`, malformed and unsupported fields, generation/timestamp,
  explicit jump, and deterministic repeatability. No extent math, PCM,
  SOFA/BRIR, playback, DRC, FFmpeg runtime, CUDA, or endpoint behavior is
  claimed. Focused command/evidence is under `tmp/r1b1`.

## Status refresh: 2026-08-27 (Renderer R1B2 position/batch bridge; bounded PASS)

- **Conversion contract**: The bridge uses the documented OAMD room axes and
  explicit Gate7B geometry policy to map normalized standard position to
  listener-relative Windows axes and then maps to the SceneAdapter/System H
  `[front,right,up]` vector as `(listenerY-roomY, roomX-listenerX,
  roomZ-listenerZ)`, then normalizes to `UnitVector3`.
  It requires `OamdCartesian`, positive finite room dimensions, and a listener
  inside the normalized room; it does not clamp or invent a listener policy.
  Effective B2b room coordinates preserve finite distance projection; screen-
  anchored and infinite-distance records are rejected without screen geometry,
  and inactive objects produce zero linear gain.
- **Batch boundary**: Gate6C records are passed through the R1B1 property
  contract and emitted as one deterministic decoded `targetGroup` for a single
  co-timed metadata group only when
  object IDs are strictly ordered and timestamps/ramp durations agree. Polar,
  empty/mixed batches, zero relative vectors, malformed properties, and
  `EXTENT_PENDING` records are rejected without partial output. A zero ramp is
  accepted for the decoded-target scheduler's immediate-step policy; this handles one
  co-timed metadata group, not an arbitrary multi-block stream.
- **Evidence**: `Eac3SceneObjectPropertiesProbe` passes 44 cases, including
  room-to-listener axis conversion, batch assembly, polar rejection, and
  non-zero extent rejection. `Eac3OamdB2bProbe` passes 37 cases. No PCM,
  WASAPI, DRC, FFmpeg/libav, CUDA, or endpoint behavior is claimed.

## Status refresh: 2026-08-27 (Renderer R1B3 decoded-target ramp scheduler; bounded PASS)

- **Timing blocker**: Gate6C `rampDuration` starts at the state evaluated at
  `sourcePosition` and reaches its target at ramp end. SceneAdapter stores a
  timestamped point as a target and interpolates toward the next point;
  duration currently only bounds the pushed block.
- **Implementation**: `SceneObjectRampScheduler` evaluates decoded targets
  independently of SceneAdapter: first state snap, zero-duration immediate
  step, non-zero ramp from state-at-S to target-at-S+D, overlap replacement,
  jump-position-only behavior, Cartesian interpolation, and exact endpoint.
- **Grouping boundary**: The scheduler accepts one decoded target group at a
  time; stream grouping and incremental commit orchestration remain explicitly
  pending. It does not claim `SceneAdapterUpdate` readiness or whole-stream
  rollback.
  Non-zero extent remains unsupported.
- **Evidence**: `Eac3SceneObjectRampSchedulerProbe` prints one named assertion
  per bounded behavior and a `SchedulerSelfTest` summary, including decoder
  target-group seam, exact Cartesian values, overlap/jump, listener crossing,
  validation, generation/reset/stale, late-group transactionality, and
  prefilled-snapshot clearing. This does not claim PCM, WASAPI, DRC,
  FFmpeg/libav, CUDA, or whole-stream rollback.

## Status refresh: 2026-08-27 (Renderer R1B4 causal metadata grouping; bounded PASS)

- **Grouping**: `SceneObjectStreamGrouper` converts an ordered Gate6C metadata
  vector into consecutive same-timestamp/ramp decoded target groups with
  whole-call transactional output. It preserves generation, last emitted
  timestamp, and coordinate-policy invariants across calls; reset is strictly
  monotonic. Same-timestamp/different-ramp input is explicitly unsupported.
- **Feeding**: `applyNextCausalGroup` applies at most one next group when the
  caller's current sample has reached its timestamp and never auto-applies a
  future group. Caller-applied groups are an incremental transaction boundary;
  later failures cannot roll them back.
- **Evidence**: `Eac3SceneObjectStreamGrouperProbe` covers 15-object ordering,
  zero/nonzero groups, ordering/timestamp failures, late transactional
  rejection, generation/reset/stale, extent unsupported, and no-future causal
  feeding. PCM, DRC, WASAPI, FFmpeg/libav, CUDA, and playback remain out of
  scope.

## Status refresh: 2026-08-27 (Renderer R1B5 System H gain-frame seam; bounded PASS)

- **Frame contract**: `makeSystemHGainFrame` consumes one evaluated scheduler
  snapshot and emits per-object identity plus 22 configured System H speaker
  gains and a finite summed vector. It validates strict unique object IDs,
  finite nonnegative gains, unit positions, and panner validity.
- **Transaction and silence**: Empty snapshots are accepted as explicit
  silence. Zero-gain objects remain in the frame with all-zero speaker gains;
  malformed or late failures clear all output. Panner output is scaled so
  each object's speaker power equals object gain squared within tolerance.
- **Integration evidence**: `Eac3SceneObjectGainFrameProbe` passes 18 named
  cases covering two-object deterministic frames and the causal Gate6C
  grouper → scheduler snapshot → gain-frame path at exact start/mid/end,
  including no-future and stale handling.
  No PCM, DRC, WASAPI, FFmpeg/libav, CUDA, SOFA, or playback behavior is
  claimed.

## Status refresh: 2026-08-27 (Renderer R2A1 SOFA cache extractor/loader; bounded PASS)

- **Asset**: The ignored local BBC R&D asset
  `docs/dev/reference-cache/renderer-assets/bbcrdlr_systemH.sofa` exists at
  464,099,203 bytes, has an HDF5 signature, and matches the manifest SHA-256
  `09dc3414a5eb7d9a325e0ad750da87ce63c9d4baf270c980296c726e152c89fa`.
  Provenance and the recorded `MultiSpeakerBRIR` dimensions/license are in
  `docs/dev/eac3-joc-renderer-assets.md`; the binary remains local-only.
- **Implementation**: A one-shot h5py extractor validates exact SOFA 1.0 /
  MultiSpeakerBRIR 0.3 / FIRE metadata, selects unique identity listener M=0,
  maps receiver +Y to left and -Y to right, and maps all 22 emitters by
  explicit BS.2051 azimuth/elevation matching. The native C++ loader consumes
  only a versioned little-endian cache; no HDF5 runtime is linked.
- **Evidence**: Real cache generated at `tmp/r2a-system-h-brir.cache` and
  loaded by `Eac3SofaBrirCacheProbe`, which passes 17 synthetic/real-cache
  cases including shuffled mapping, payload-order contract, bad header/count,
  duplicate mapping, truncation, trailing bytes, non-finite IR, and hash-field
  validation. Full metadata inspection remains at `tmp/r2a-sofa-inspection.txt`.
- **Boundary**: Cache is local-only and no SOFA/IR data enters Git. No
  convolution, PCM, playback, DRC, FFmpeg/libav, CUDA, or WASAPI behavior is
  implemented.
- **PE audit command**: using `C:\Program Files (x86)\Microsoft Visual
  Studio\18\BuildTools\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\dumpbin.exe`,
  run `dumpbin.exe /DEPENDENTS build-mm\Debug\Eac3SofaBrirCacheProbe.exe`
  (and the GainFrame/StreamGrouper/RampScheduler probe executables) and reject matches
  for `avcodec|avformat|avutil|swresample|cuda|wasapi|winmm`.
- **Validation evidence**: `scripts\validate-all.ps1 -BuildDir build-mm
  -Configuration Debug` passed unit tests, report schema, and smoke. Aggregate
  report: `build-mm\validation-report.json`; latest smoke log/report:
  `build-mm\cache\logs\player-smoke-20260827-180139-315-404bb524.log` and
  `player-smoke-20260827-180139-315-404bb524.report.json`. Focused commands
  passed Python `--self-test`, real extraction, C++ `--self-test` (17 cases),
  and real-cache integration (17 cases). PE `/DEPENDENTS` forbidden-import
  checks passed for all four probes.

## Status refresh: 2026-08-27 (Renderer R1B Objects extent audit; INCONCLUSIVE)

- **Normative audit**: The local `ITU-R-BS.2127-1-2023.pdf` contains the
  complete Objects timing/gain and extent path in §7.2, §7.3.8, and §7.3.11,
  including the direct/diffuse split, virtual-source aggregation, and final
  normalization. This is enough to implement a reviewed extent primitive, but
  not enough to infer a missing decoder-to-scene contract.
- **Input audit**: `B2bObjectState` exposes normalized `{width, depth, height}`
  size values from the OAMD codewords, but the current `SceneAdapter` input
  carries only a unit point direction and linear gain. It has no cartesian
  flag, explicit extent presence, diffuse value, or object-divergence state;
  Gate6C metadata retains B2b state but has no adapter into SceneAdapter.
- **Decision/scope**: R1B remains read-only `INCONCLUSIVE`; no non-zero extent
  code was added and no missing fields are silently treated as zero. The next
  safe slice is a decoder-to-scene extent contract followed by a standalone
  §7.3.8 or §7.3.11 primitive. No PCM, decorrelation, SOFA/BRIR, playback,
  DRC, FFmpeg/libav, CUDA, or SIMD changes were made.
- **Evidence**: `tmp/r1b/spec-field-matrix.txt` records the field mapping,
  units/ranges, exact local PDF page/section references, and the stop reason.

## Status refresh: 2026-08-27 (Renderer R1A SceneAdapter; bounded PASS)

- **Implementation**: Added the renderer-neutral `SceneAdapter`, which maps
  the seven non-LFE bed labels explicitly to the corresponding fixed System H
  speakers and calls the R0D configured point-source engine for object
  positions. It emits per-object 22-channel gain frames before summing them;
  LFE is a separate enabled/gain field and never enters point-source panning.
- **Timing/lifecycle**: Block-timestamped object gain and position updates are
  linearly interpolated between known points; `jumpPosition` suppresses only
  position interpolation at its target. Missing object updates are retained as
  historical half-open metadata-gap intervals with HoldLast or Terminate
  policy, so reappearance cannot make a past gap interpolate. Reset advances
  generation and clears bed/object/timeline state; stale generations,
  non-monotonic or overflowing timestamps, duplicate identities, non-unit
  positions, and non-finite/negative gains fail closed. Extent remains
  unapplied and DRC is off.
- **Self-test/build**: `Eac3SceneAdapterProbe` passed 34 cases covering all
  bed routes, separate LFE on/off, static/multiple objects, gain and position
  interpolation, jumpPosition, both gap policies including historical gap
  reappearance queries and no-cross-gap interpolation before the gap,
  reset/generation/stale rejection, invalid inputs, and deterministic output.
- **Scope/limits**: This is a gain-frame adapter only. It does not mix PCM,
  apply extent, perform SOFA/BRIR convolution, access playback/WASAPI, or add
  FFmpeg/libav/CUDA/SIMD dependencies. R1B owns extent semantics.

## Status refresh: 2026-08-27 (Renderer R0C nominal-to-actual System H; bounded PASS)

- **Normative mapping audit**: The local BS.2127-1 text at §6.1.3.1 requires
  separate nominal and real Cartesian position lists, nominal convex-hull
  topology, and Triplet/Quad region construction using corresponding real
  positions. It requires the bottom virtual speaker and only requires an
  upper virtual speaker when T+000 is absent. The fixed System H includes
  T+000. The precondition data shape is the BS.2051-labelled channel record:
  real position, nominal position, and LFE identity; this slice accepts only
  the 22 non-LFE System H point-source labels.
- **Implementation**: Added the dependency-free
  `bs2127-nominal-to-actual.h/.cpp` adapter and probe. Input records are keyed
  by `Bs2051Label`, so order permutation is deterministic; every label is
  required exactly once. Real vectors must be finite, unit length, pairwise
  distinct, and inside the local BS.2051 System H Table 10 azimuth/elevation
  ranges. The existing nominal catalog and region order are reused; supplied
  real vectors are passed only to Triplet, Quad, and lower VirtualNgon solve
  paths. Empty input is the strict nominal==actual identity case.
- **Direct-virtual audit**: §6.1.3.1.1 is not empty for fixed System H.
  `Su` excludes T+000 and has maximum absolute azimuth 180 degrees, so
  `L_u=220` and no upper direct virtual is created. `Sl` is B+000/B+045/B-045,
  so `L_l=85`; five lower direct-downmix virtuals are required for M+135,
  M-135, M+180, M+090, and M-090. Their nominal elevation is -30 degrees,
  real elevation is mean(Sl)=-30 degrees, real azimuth follows the source
  mid speaker, and each maps one-to-one to that mid speaker. R0C records the
  real layout; R0D adds these derived topology entries. The generic forced
  lower pole remains separate.
- **Self-test**: `Eac3Bs2127NominalToActualProbe` passed 15 cases, covering
  strict nominal identity, reversed label order, one allowed speaker
  displacement, displaced-region finite/power-normalized output, boundary,
  lower VirtualNgon, missing/duplicate/out-of-range/non-unit fail-closed
  layouts. Existing `Eac3Bs2127PointSourcePannerProbe` remained PASS (92
  cases; 32,768 deterministic samples; unsupported=0; invalid=0; power 1).
- **Scope/limits**: This is a bounded fixed System H configuration seam, not
  a generic layout loader or a full renderer. It does not implement nominal
  virtual-speaker derivation for arbitrary layouts, scene/extent routing,
  LFE point-source panning, SOFA/BRIR, convolution, PCM, playback, JOC, or
  DRC; no FFmpeg/libav runtime dependency is introduced.
- **Evidence**: Spec extraction is under `tmp/r0c/`; direct virtual derivation
  is recorded in `tmp/r0c/direct-virtual-system-h-audit.txt`; the focused
  probe output is `tmp/r0c/nominal-to-actual-selftest.log`. The adapter target has no
  forbidden FFmpeg/libav imports.

## Status refresh: 2026-08-27 (Renderer R0D configured System H direct virtuals; bounded PASS)

- **Implementation**: Added the separate 28-point
  `Bs2127SystemHConfiguredPanner` catalog: 22 physical System H points, the
  five §6.1.3.1.1 direct-downmix lower virtuals, and the forced lower pole.
  The legacy 23-point diagnostic catalog remains compatible. Direct virtual
  facets participate in nominal hull construction and real Triplet/Quad
  solves; virtual coefficients map one-to-one to the corresponding physical
  mid speaker. The generic lower-pole VirtualNgon includes the five direct
  virtuals in its eight-point ring, applies the existing 1/sqrt(n) downmix,
  and performs final physical power normalization. Ordinary configured facets
  are dispatched in one stable catalog-wide first-valid order; only after
  that pass does the forced lower-pole VirtualNgon run.
- **Self-test**: `Eac3Bs2127SystemHConfiguredPannerProbe` passed 21 cases:
  exact five-point set and target labels, 28-point hull, maximum four-edge
  facets, physical identities, direct one-to-one gain routing, lower pole,
  displaced lower-layer mean-elevation/azimuth derivation, invalid-layout
  rejection, independent first-valid catalog-order checks (including a direct
  facet interior before a later real-only facet), deterministic finite
  power-normalized output, and 32,768 global sphere samples with unsupported=0
  and invalid=0.
- **Scope/limits**: This is fixed System H only. It does not claim a generic
  direct-virtual layout algorithm, scene/extent routing, LFE panning,
  SOFA/BRIR, convolution, PCM, playback, JOC, DRC, or FFmpeg/libav runtime.
  R1 still owns scene-to-speaker integration.
- **Evidence**: `tmp/r0d/configured-panner-selftest.log` and
  `tmp/r0d/pe-dependents.log`; normative derivation remains in
  `tmp/r0c/direct-virtual-system-h-audit.txt`.

## Status refresh: 2026-08-27 (Renderer R0B point-source dispatcher; bounded PASS)

- **Implementation**: Added the owned
  `Bs2127SystemHPointSourcePanner`, which dispatches in BS.2127-1 §6.1.1
  first-region-valid order: stable nominal System H identity/Triplet/Quad
  selector first, then the existing lower-pole VirtualNgon. All paths expose
  one renderer-neutral 22-speaker gain vector; LFE remains outside the point
  source layout and DRC remains off.
- **Self-test**: `Eac3Bs2127PointSourcePannerProbe` passed 92 cases, including
  all 22 identities, 26 Triplet and 4 Quad facet interiors, lower-pole
  VirtualNgon, axes/poles, finite power-normalized output, NaN/non-unit
  rejection, and 32,768 deterministic Fibonacci-sphere samples. The sample
  run reported `unsupported=0`, `invalid=0`, `minPower=1`, `maxPower=1`; a
  second call for every sample was byte-stable.
- **Scope/limits**: This closes only nominal System H region dispatch. It does
  not implement nominal-to-actual adaptation, scene/extent routing, SOFA,
  BRIR, convolution, PCM, playback, or generic E-AC-3 support. The global
  coverage claim is bounded to the fixed catalog and tested sample set; no
  heuristic fallback is used.

## Status refresh: 2026-08-27 (Renderer R0A BS.2127 QuadRegion; bounded PASS)

- **Implementation**: Added the standalone scalar
  `tools/atmos-render/bs2127-quad.h/.cpp` primitive and extended the nominal
  System H selector to query real Triplet and Quad facets in stable topology
  order. Quad vertices retain sorted topology identities and additionally
  expose a deterministic perimeter order, anticlockwise as viewed from the
  listener. The solver implements BS.2127-1 §6.1.2.3.1-.3 equations (1)-(4),
  including real-root selection, same-direction validation, and final power
  normalization.
- **Self-test**: `Eac3Bs2127SelectorProbe` passed 70 cases: the four current
  System H Quad interiors select through the unified result shape, synthetic
  centre/boundary/corner/cyclic-order cases pass, degenerate duplicate input
  fails closed, and the existing Triplet/topology/identity/boundary tests
  remain covered. `VirtualNgon` remains its separate lower-pole region and is
  not triangulated as a Quad.
- **Scope/limits**: This is nominal System H point-source gain math only. It
  does not implement nominal-to-actual adaptation, scene routing, LFE point
  panning, extent, SOFA/BRIR, convolution, PCM, playback, DRC, or a generic
  E-AC-3 renderer. Numerical tolerances and perimeter ordering are explicit
  implementation policies; the Quad equations are the local normative basis.

## Status refresh: 2026-08-27 (J0A2B Gate 5A configuration-policy closure; bounded PASS)

- **Gate 5A policy correction**: The parser's stale `config >= 2` phase gate was
  removed. Defined downmix configurations 0-4 now return Pass after complete
  bounded syntax/Huffman/padding validation, with channel counts 5, 7, 7, 5,
  and 7 respectively. Reserved configurations 5-7 remain structured
  Unsupported. No phase compensation, PCM, renderer, or DRC behavior changed.
- **J0A2 qualification**: On the supplied config-3 fixture, payload 11 (OAMD
  B1) and payload 14 (Gate 5A) both parse Pass, so the native qualifier reports
  one Qualified frame per accepted AU. The old access-unit oracle reports the
  same payload counts and `jocParsedPassCount=1000`, with no JOC Unsupported
  results. The native qualifier remains bounded to exact config-3 J0A1 input;
  its Gate 5A policy result does not imply native config-4 core support.
- **Scope/limits**: This is removal of an obsolete qualification policy gate,
  not a new phase algorithm. Configurations 5-7, malformed syntax, legacy or
  dependent streams, and unsupported advanced branches remain fail-closed;
  there is still no PCM, renderer, playback, JOC reconstruction, or DRC.

## Status refresh: 2026-08-27 (J0A3 native config-3 core/JOC session bridge; bounded PASS)

- **Strict dual-input bridge**: Added the FFmpeg-free
  `NativeEac3JocSessionBridge` and focused probe. Each exact config-3 AU is
  processed by N5B and J0A2; construction of existing JOC/QMF/session input
  requires matching AU index, timestamp, ordinary core content, and 1,536
  samples. Core order `FL,FC,FR,SL,SR,LFE` is explicitly mapped to
  `FL,FR,FC,SL,SR`, with LFE passed separately and DRC off.
- **Real metadata path**: OAMD payload 11 is converted using public B1, B2A,
  and stateful B2B APIs. The resulting Gate6C metadata contains 15 dynamic
  object updates per AU; no metadata is inferred from JOC counts. Session
  callbacks report finite 15-object/LFE batches and preserve the separate
  256-sample EOS tail.
- **Config3 evidence**: 10/100/1000-AU runs accepted and qualified every AU;
  the 1000-AU run had 1,000 session frames, 15,000 metadata updates, 1,001
  callback batches, 1,536,000 ordinary samples/channel, and a 256-sample EOS
  tail. Repeated output was byte-identical with digest
  `15bd523357983ba2`.
- **Oracle boundary**: The existing Gate6C diagnostic run agrees on the
  10-AU association, 11 batches, 150 metadata updates, 15-object/LFE sample
  counts, and finite output metrics. Its FFmpeg-backed float path is a separate
  diagnostic oracle; the native and oracle float digests are not claimed
  bit-identical.
- **Lifecycle/limits**: Selftest covers callback rejection, poisoned flush,
  reset recovery, and EOS. The core tail is never submitted as a metadata AU.
  The bridge is bounded to config3 and remains renderer-neutral; config4,
  legacy/dependent streams, advanced syntax, playback, phase filtering, and
  DRC remain unsupported/out of scope.

## Historical status: 2026-08-26 (Gate J0A2 native qualification boundary; superseded by J0A2B)

- **Qualification seam**: Added `Eac3NativeJocQualifierProbe` and the
  FFmpeg-free `native-eac3-joc-qualifier.{h,cpp}` layer. It invokes J0A1,
  requires exactly one payload 11 and one payload 14, and reuses public
  `eac3oamd::parseB1()` and `eac3joc::parsePayload()` APIs. Source payload IDs,
  bit bounds, typed parser reports, AU index, and zero-based 1,536-sample
  timestamps remain visible; no JOC metadata is reconstructed and DRC stays
  off.
- **Observed config-3 result**: OAMD payload 11 was Gate 6B1 Pass on every
  tested AU. Gate 5A parsed payload 14 but returned
  `recognized-config-phase-unsupported` for downmix config 3, so the layer
  correctly reports `qualifiedJocFrames=0`, `jocUnsupportedCount=N`, and emits
  no qualified `FrameReport`. This is deliberate fail-closed behavior, not a
  claim that the fixture qualifies for JOC.
- **Evidence**: Native runs for 10/100/1,000 AUs are under `tmp/j0a2/`; the
  1,000-AU digest is `855a87f9d206b5f0` and repeat output is byte-identical.
  The old offline oracle independently reports
  `jocParsedPassCount=0`, `jocUnsupportedCount=1000`, and
  `jocDownmixConfigs=3:1000` in
  `tmp/j0a2/old-oracle-config3-1000-summary.log`. Selftest covers qualified
  synthetic input, missing/duplicate/wrong-ID/malformed payloads, poison, and
  reset (7 cases).
- **Limits**: No PCM, renderer, playback, JOC scene, DRC, advanced syntax, or
  generic E-AC-3 qualification is included. A future slice must explicitly
  extend the accepted Gate 5A configuration before claiming a qualified
  report for this fixture.

## Status refresh: 2026-08-26 (Gate J0A1 native EMDF extractor; bounded PASS)

- **Native extraction boundary**: Added `Eac3NativeEmdfProbe` and the
  FFmpeg-free `native-eac3-emdf.{h,cpp}` component. It reuses native
  `parseSyncframe()`/`FrameHeader`, accepts exactly one complete config-3
  type-0 independent SID0 48 kHz six-block frame with no trailing bytes, and
  returns bounded EMDF container records with version/key, protection,
  timing/group, priority/procAllowed, payload header/data bit bounds and
  covered byte bounds, plus unchanged raw payload bytes. It does not qualify
  JOC or apply DRC.
- **Strict scan behavior**: Only fully parsed `0x5838` candidates are emitted;
  container length and frame limits are checked, protection/padding must pass,
  and the scanner advances to each accepted container end to preserve
  non-overlap. False candidates and malformed candidates are skipped during
  frame scanning; the exact parser helper returns structured Malformed for
  truncation, bounds, protection, or padding failures.
- **Config-3 evidence**: Native runs over 10, 100, and 1,000 AUs each passed
  with one EMDF container per AU and payload IDs `1,2,11,14` exactly once per
  AU. Deterministic payload digests were
  `5967abe8ec673d7f`, `df101ade73b8eb81`, and `f5f017cd379fbbf1`; repeat-run
  logs are under `tmp/j0a1/`. The existing FFmpeg-backed oracle independently
  reported matching container/payload counts and IDs for the same input.
- **Limits**: Raw payloads 11/14 are delivered but not interpreted here;
  JOC qualification, PCM, DRC, renderer/playback, and generic AU assembly are
  future work. Legacy, dependent/additional/type-2, and non-config3 topology
  inputs remain structured Unsupported.

## Status refresh: 2026-08-26 (Gate 8N-5B native core seam; bounded PASS for config-3)

- **Seam contract**: Extended the existing callback seam with additive
  `Disposition::OrdinaryEac3` and `CoreContentKind`. The new
  `NativeEac3CoreDecoderSeam` adapts the N5A native decoder while preserving
  the old Fake/JOC `Disposition::Joc` behavior. Successful ordinary native
  decode is therefore never labeled Joc before any JOC metadata qualification.
- **Frame/tail state**: Callback frames expose coded-order IDs
  (`FL,FC,FR,SL,SR,LFE`), explicit LFE ownership, finite planar float PCM,
  `sampleCount=1536`, and `drcApplied=false`. `flush()` exposes one separate
  256-sample `eosTail` frame after accepted AUs (or an explicit empty flush
  marker for an empty stream); it is not counted as a normal AU.
- **Lifecycle/backpressure**: Adjacent syncframes remain continuous. Native
  parse/PCM failure or callback rejection poisons the seam until `reset()`;
  cancellation and repeated EOS are structured flow results. The focused
  selftest passed five cases: ordinary disposition, poison/reset, empty EOS,
  cancel, and strict ordinary flush shape. Config-3 10-frame run passed 10 contiguous AUs, 15,360 core
  samples plus a 256-sample tail per channel, channel identity, finite output,
  and EOS accounting; its seam-interface float digest was
  `944403dc83299e9f` in two runs. A 100-frame run passed with 153,600 core
  samples plus the same tail (`4b0043034fceef8f`); callback-rejection run
  passed fail-stopped backpressure.
- **Timestamp contract**: The seam preserves the caller's external sample
  timestamp, including a non-zero base, while passing the native decoder its
  own contiguous zero-based cursor. EOS uses the next external timestamp;
  ordinary flush frames reject any non-zero sample count unless marked as the
  separate 256-sample tail.
- **Topology boundary**: Raw config-4 fails on its first Legacy-AC-3 frame with
  `Unsupported`, zero callback PCM, and zero accepted frames. This adapter
  remains bounded to the supplied config-3 type-0 independent SID0 six-block
  topology; dependent/legacy presentation assembly, advanced syntax, JOC,
  renderer/playback, and DRC remain out of scope.
- **Focused commands**: `cmake --build build-gate8a --config Release --target
  Eac3NativeCoreDecoderSeamProbe -j 4` and the seam executable runs recorded
  under `tmp/n5b/` provide the current evidence. The target has no
  libav/FFmpeg runtime dependency.

## Status refresh: 2026-08-26 (Gate 8N-5A native core adapter; bounded PASS)

- **Adapter contract**: Added the standalone FFmpeg-free
  `NativeEac3CoreDecoder` in
  `tools/atmos-joc-probe/native-eac3-core-decoder.{h,cpp}`. It reuses the
  existing `parseSyncframe`, `parseEac3Bsi`, and `Eac3PcmSession` path and
  emits `DecodedAccessUnit` values with contiguous `sampleStart`, exactly
  1,536 core samples, 48 kHz, stable coded channel IDs
  (`FL,FC,FR,SL,SR,LFE`), explicit LFE identity, finite per-channel samples,
  and `drcApplied=false`. Adjacent AUs retain per-channel reference dither and
  transform overlap; reset is explicit and never implicit at an AU boundary.
- **EOS/failure semantics**: `flush()` reports a separate 256-sample tail per
  channel, never as a normal AU. Empty EOS is valid after open; repeated EOS,
  cancellation, post-EOS input, and poison state are explicit. Any parse,
  topology, PCM, or finite-output failure poisons the adapter until `reset()`.
  Selftest passed four cases covering config open, poison/flush rejection,
  reset/empty EOS, and cancellation.
- **Bounded acceptance**: Config-3 10-frame and 100-frame runs both passed
  sample timeline, channel identity, finite PCM, overlap/dither continuity,
  and EOS accounting. The 10-frame run was byte-identical across two runs and
  reports `pcmDigest=83dd39524add2934`, matching the prior N3B config-3
  reference digest; it emits 15,360 core samples plus one 256-sample tail per
  channel (15,616 total/channel). The 100-frame run reports
  `pcmDigest=f19e56d3fc7fe59` and 153,856 total samples/channel. Evidence is
  under `tmp/n5a/config3-10-run1.log`, `config3-10-run2.log`, and
  `config3-100-run1.log`/`run2.log`.
- **Fail-closed topology gate**: Raw config-4's first frame is Legacy AC-3;
  the adapter returns `Unsupported` (`acceptedFrames=0`, no PCM) immediately.
  Evidence: `tmp/n5a/raw-config4-first-au.log`. Dependent/additional/type-2,
  sample-rate/topology changes, rematrix/coupling, advanced syntax, DRC,
  JOC, playback, and FFmpeg/libav runtime remain out of scope.
- **Interface boundary (superseded by N5B below)**: The N5A standalone adapter
  is still directly reusable, while N5B supplies the additive ordinary seam
  contract. No existing JOC or playback path was changed.

## Status refresh: 2026-08-26 (Gate 8N-4A substream inventory; bounded PASS)

- **Implementation boundary**: Added the FFmpeg-free
  `Eac3NativeSubstreamProbe` in
  `tools/atmos-joc-probe/native-eac3-substream-probe.cpp`. It reuses
  `parseSyncframe`, `parseEac3Bsi`, and the existing transactional
  `AccessUnitAssembler`; it records ordered type/SID, normalized byte and bit
  boundaries, sample rate, block count, frame size, `acmod`, LFE, coded channel
  count, and dependent `chanmape/chanmap` location order. It does not duplicate
  the parser, mix channels, render, apply DRC, or link FFmpeg/libav. Its
  focused self-test reports five cases, including a real Legacy-AC-3 base plus
  dependent sequence and the `0xA010` coded-location order.
- **Normative association**: TS 102 366 V1.4.1 §E.1.3.1.1-.8 defines type 0
  independent, type 1 dependent, type 2 AC-3-independent, the required
  independent ID 0, and immediate dependent ordering; the same clause says an
  AC-3 stream present in E-AC-3 is treated as independent ID 0. Section
  §E.2.8.1-.4 defines independent-plus-dependent programme association and
  replacement/supplement semantics; §E.1.3.1.8 defines `chanmap` bit order
  and channel-location pairs. Section §F.2 defines an Enhanced AC-3 sample as
  enough syncframes to deliver six blocks from every present substream.
- **Config-3 evidence**: 1,000 AUs / 1,000 frames, all E-AC-3 type 0 SID 0,
  48 kHz, six blocks, 3,072-byte frames, `acmod=7`, LFE on, six coded
  channels. `topologyDigest=0xc65c9265e06d50e3` and
  `tmp/n4a/config3-1000.log` contain every ordered AU/frame record.
- **Raw config-4 evidence**: 1,000 AUs / 2,000 frames, exactly 1,000 legacy
  AC-3 SID 0 plus 1,000 E-AC-3 dependent SID 0. Both are 48 kHz and six
  blocks; the observed first AU is legacy 2,560 bytes (`acmod=7`, LFE on,
  six channels) followed by dependent 4,096 bytes (`acmod=5`, LFE off,
  four channels, `chanmap=0xA010`, `L,R,Vhl/Vhr`, weight four).
  The summary reports `validBaseSid0=YES`, `baseSid0Frames=1000`,
  `legacySid0Frames=1000`, and `presentationBase=legacy-ac3-sid0`;
  `eac3IndependentMain=NO` therefore means no E-AC-3 type-0 main, not no
  valid base. `topologyDigest=0xAB08CF00D8C2E2D3`; evidence is
  `tmp/n4a/raw-config4-1000.log`.
- **DEE matrix**: Each of
  `powder_active_5s_1152k.eb3`, `1280k.eb3`, `1408k.eb3`, `1512k.eb3`,
  `1536k.eb3`, and `1664k.eb3` passed 1,000 AUs / 2,000 frames with the same
  legacy-ID0 plus dependent-ID0 topology, no E-AC-3 type-0 independent main
  frame but `validBaseSid0=YES`/`presentationBase=legacy-ac3-sid0`, no BSI
  failures, and 48 kHz/six-block shape. Digests are respectively
  `0xAF5F3E8C054A3583`, `0x07E2CD61F32FB7A3`, `0x6472AB854FB14553`,
  `0xFB513706C38F1A13`, `0xDA2839991025B583`, and
  `0xAB08CF00D8C2E2D3`; individual full logs are under `tmp/n4a/`.
- **Decision/evidence limit**: The shared bounded assembler accepts these
  pairs because AC-3 is explicitly treated as independent ID 0 and each
  dependent immediately follows it with matching rate/blocks. This is not an
  adjacency-only guess. It is a supplied-matrix inventory/association result,
  not a generic E-AC-3 channel assembler: no container timestamps, additional
  independent programmes, dependent replacement, renderer speaker routing,
  or advanced syntax are implemented. `eac3IndependentMain=NO` is explicit
  for raw config-4 and all six DEE variants.

## Status refresh: 2026-08-26 (Gate 8N N3A-FIX/N3B-C2 transform alignment; PASS for bounded reference)

- **Correction**: TS 102 366 V1.4.1 §6.9.4.1 Step 5 specifies
  `x[2*n+1] = yr[N/8-n-1]`, which is `yr[63-n]` for N=512. The previous
  reference and its independent evidence scripts used `yr[127-n]` at this
  one slot. The final odd quadrant correctly remains `yr[N/4-n-1]`, or
  `yr[127-n]`. The old long oracle and N3B PCM digests derived from that
  shared transcription are invalid and are superseded below.
- **Reference/selftest**: `Eac3NativeTransformProbe` reports
  `selfTest=PASS cases=12`, including a discriminator vector whose expected
  odd sample fails under the former index. Independent TS long and short
  dump comparisons remain `maxAbsError=0`, tolerance `1e-12`, exit 0. The
  corrected evidence is under `tmp\\n3a\\long-layer-compare-postfix.log`.
- **Pinned av_tx evidence**: The actual basis matrix maps the corrected long
  first quadrant as `x0=-raw127`, `x1=-raw126`, ... with worst row residual
  `1.8591372763498981e-7`; five-vector linearity residual is
  `1.7816277352866727e-6` (short `6.9906869332170274e-7`). Complete relation
  and duplicate/missing accounting are in
  `tmp\\n3b-c\\basis-relationship-postfix.log`.
- **Packed temporal result**: With actual basis matrices, pinned
  `vector_fmul_window_c`, FF KBD window, 12 blocks plus a zero tail block,
  all four sequences produce 3328 samples on both sides, select shift 0, and
  close within bounded float/window comparison. Observed
  `maxAbs/RMS/referencePeak/relativePeak` are respectively
  `long-long 0.00083510823914423327/4.5089825945178606e-05/176.58009435009208/
  4.7293452991848953e-06`; `long-short
  0.00081681619540319161/3.7885928742868277e-05/175.24413734515682/
  4.6610186667438081e-06`; `short-long
  0.00073016699170658228/3.9590232667580811e-05/164.8039076955252/
  4.4305198943192778e-06`; `short-short
  0.00038140238657291547/3.1135333260113237e-05/90.689463486107755/
  4.2055865357648801e-06`. KBD-vs-Table-6.33 is separately
  `max=4.9882395724877571e-6`, `RMS=2.6957196095541617e-6`. Full shift,
  zero-control, and EOS evidence is in
  `tmp\\n3b-c\\actual-packed-sequences-postfix-eos.log`.
- **Conclusion**: N3A transform temporal packing is **PASS** for the tested
  scalar/reference and pinned packed-window evidence. This is an observed
  float `av_tx`/KBD-vs-rounded-Table-6.33 comparison, not a bit-exact claim.
  N3B remains a bounded
  diagnostic PCM session, not production or generic E-AC-3 acceptance; its
  corrected 10-frame prefix outputs are finite/deterministic with continuous
  overlap and dither state (logs under `tmp\\n3b\\`). Advanced syntax,
  dither sequence equivalence, rematrix/coupling, DRC, substream assembly,
  JOC, renderer, playback, CUDA, SIMD, and FFmpeg runtime linkage remain out
  of scope.
- **Corrected N3B prefix evidence**: Config-3 (10 accepted E-AC-3 frames)
  reports `stateDigest=94d37a26243b746f`,
  `pcmDigest=83dd39524add2934`, `observedSamplesPerChannelIncludingEosTail=15616`;
  raw config-4 (10 accepted plus 10 legacy companions) reports
  `stateDigest=bb3aa29ad608c831`, `pcmDigest=803dd5274a7c84b3`, and the same
  15616 samples/channel. Both runs per input are byte-identical; logs are
  `tmp\\n3b\\config3-10frame-n3afix-run1.log`/`run2.log` and
  `tmp\\n3b\\raw-config4-10frame-n3afix-run1.log`/`run2.log`. Each reports
  `allFramesSixBlocks=PASS`, `ditherContinuity=PASS`,
  `overlapContinuity=PASS`, `eosTail=PASS`, and `probeResult=PASS`.

## Status refresh: 2026-08-26 (Gate 8N N3B-D real-stream PCM comparison; INCONCLUSIVE)

- **Export/policy**: The existing PCM-session probe now supports explicit
  diagnostic-only interleaved f32le `--dump-f32 path` export. The
  production/reference default is unchanged (`ditherMode=reference`);
  `--dither zero` is explicitly `NONCONFORMING_ORACLE_ONLY` and exists only
  for sensitivity/bounding comparison. It cannot separate FFmpeg dither,
  native dither, and low-level quantization contributions.
  Native config-3 output covers 10 continuous frames plus the one 256-sample
  EOS tail (15616 samples/channel, 48 kHz). FFmpeg was run offline with
  `-drc_scale 0` over the same 10-frame core (15360 samples/channel).
- **Channel/timeline mapping**: Native coded order is observed as
  `FL,FC,FR,SL,SR,LFE`; FFmpeg output is `FL,FR,FC,LFE,SL,SR`, so comparison
  maps native indices `[0,1,2,3,4,5]` to FFmpeg `[0,2,1,4,5,3]`. The native
  EOS tail is excluded from the core comparison; integer shift search is
  bounded to -256..+256.
- **Reference-dither comparison**: Active FL/FR best shift is 0, scale is
  `1.0000003865228684`/`1.000000405960954`, correlation is
  `0.99999992730195486`/`0.99999984986985824`, and RMS error is
  `5.9743255292067466e-06`/`5.9455088440285751e-06` (SNR
  `68.38`/`65.23` dB). Surround channels are low-level and show dither/
  quantization sensitivity; the LFE is silent in this prefix. Full per-channel
  peak/DC/correlation/shift/scale/max/RMS/SNR data is in
  `tmp\\n3b-d\\compare-reference-mapped.log`.
- **Zero-dither sensitivity**: The explicit zero mode leaves FL/FR at best shift
  0 with RMS `4.378034713302746e-06`/`4.3311053055564236e-06` and correlation
  `0.99999996096058219`/`0.99999992033143048`; FFmpeg's own dither remains
  present, so this cannot isolate FFmpeg dither from native dither or
  low-level quantization. Evidence is in
  `tmp\\n3b-d\\compare-zero-mapped.log` and the native zero-mode log.
- **Conclusion**: N3B-D is **INCONCLUSIVE** for production/native PCM
  acceptance. The real-stream comparison confirms correct 48-kHz core sample
  count, channel mapping, zero-delay alignment, and close active-channel
  numeric behavior, but dither policy and low-level/advanced branch coverage
  are not interoperably closed. No heuristic resync, silence substitution,
  DRC, rematrix, JOC, renderer, playback, CUDA, SIMD, or FFmpeg runtime path
  was added.

## Status refresh: 2026-08-26 (Gate 8N N3B diagnostic PCM session; INCONCLUSIVE)

- **Implementation boundary**: Added the standalone
  `native-eac3-pcm-session.{h,cpp}` and `Eac3NativePcmSessionProbe`. It calls
  the existing coefficient parser once per E-AC-3 frame, then feeds each
  stable coded channel (including LFE identity) to its own scalar transform
  and reference-dither state. Six blocks produce 1536 samples/channel;
  adjacent syncframes retain overlap/dither state; explicit `reset()` is for
  new stream, seek/discontinuity, or topology rebuild. Conceptual delay is
  256 samples, and EOS emits one 256-sample tail. No DRC, rematrix transform,
  JOC, renderer, playback, CUDA, SIMD, or FFmpeg/libav runtime path exists.
  Parser/transform failure poisons the session until reset; continuation after
  a partial failure is rejected.
  Raw config-4 dependent frames are diagnostic coded-channel topology only;
  substream/channel-map assembly remains outside this slice.
- **Focused/real-sample boundary**: The probe selftest and small config-3/raw
  config-4 prefixes pass finite output, channel identity, bounded parser
  bit-end, stream-reset policy, overlap continuity, and EOS-tail checks. This
  is an offline diagnostic chain only; each frame contributes 1536
  samples/channel and the stream contributes one final 256-sample
  tail/channel. It does not claim generic E-AC-3 coverage.
- **Acceptance status**: N3B remains **INCONCLUSIVE** for production/native
  PCM acceptance. The corrected N3A packed-window evidence closes the bounded
  transform alignment, but this session still does not establish generic
  E-AC-3 coverage, dither sequence equivalence, rematrix/coupling, or
  substream/channel assembly. The session preserves the TS reference contract
  and does not tune or reorder samples to force a match. Advanced syntax
  remains structured `Unsupported`.

## Historical status (superseded by the Gate 8N N3A-FIX refresh above): Gate 8N N3A scalar transform selftest PASS; external numeric cross-check INCONCLUSIVE

- **Scope**: Added the standalone `Eac3NativeTransformProbe` reference layer
  under `tools\atmos-joc-probe\`. It implements TS 102 366 V1.4.1 §6.9.4.1-
  §6.9.4.2 (PDF pages 82-86), including Table 6.33's 256-point window, long
  512-sample and switched two-256-sample inverse transforms, 256-sample
  overlap-add, factor-2 output scaling, per-channel reset, and one-shot EOS
  tail flush. It is scalar O(N²), standalone, and not connected to audblk,
  production PCM, JOC, renderer, playback, DRC, CUDA, SIMD, or FFmpeg.
- **Focused result (pre-fix)**: `build-gate8a\n3a\transform-selftest.log` reports
  `selfTest=PASS cases=11`; all-zero, impulse, all 256 single short bins plus
  EOS tail, zero-fill, alternating long/short, maximum-finite, channel
  isolation, reset, EOS, and invalid-input cases pass. Output is finite and
  deterministic.
- **Independent numeric layer (pre-fix)**: `tmp\n3a\oracle-long.log` and
  `tmp\n3a\oracle-short.log` each report 256 samples,
  `maxAbsError=0`, `firstMismatch=None`, tolerance `1e-12`, exit 0. The
  Python oracle is a separate transcription of the ETSI equations and is
  evidence-only; it is not runtime code.
- **Pinned-oracle numeric cross-check**: A temporary instrumentation of pinned
  FFmpeg commit `96f82f4fbbdc8f7525672bafbf37616ea5fd76ca` compared one
  nontrivial long and one switched vector at the post-window output layer.
  The switched vector is consistent with an approximately 0.5 FFmpeg scale
  (`bestFitActualOverExpectedScale=0.4999998937`, scaled max absolute error
  `7.303e-9`, RMS `2.116e-9`). The long vector does not yet reconcile:
  raw max absolute error is `2.7587e-3`, and the best-fit scale is `0.4290955`
  with scaled max absolute error `1.4944e-3` and RMS `2.6696e-4`.
  This is INCONCLUSIVE evidence, not a bit-exact claim; no transform tuning
  was made. Raw logs and comparison summaries are under `tmp\n3a\`.
- **Long-layer isolation**: The TS pre-rotation, 128-point complex IFFT, and
  post-rotation layers are internally finite and deterministic. At the
  synthesis boundary, FFmpeg's 256-value `av_tx` vector is not in the TS
  windowed-block order; the explicit mapping `-raw[127-2n]` for even TS slots
  and `-raw[254-2n]` for odd slots reconciles the TS unwindowed first 128
  values with max absolute error `5.65e-17`, RMS `2.25e-17`, and scale 1.
  The remaining comparison is the FFmpeg 128-sample window/delay packing
  versus the standalone TS first-256 TDAC output, so it is not a valid direct
  sample-by-sample acceptance yet. Layer evidence is in
  `tmp\n3a\ffmpeg-long-layers4.log` and
  `tmp\n3a\long-layer-compare-final.log`.
- **Delay policy**: TS §6.9.4.1 retains the conceptual 256-sample second half
  (`delay[n] = x[N/2+n]` for N=512). The reference keeps this full overlap;
  its EOS tail is exactly the output of a zero next block and is 256 samples.
  FFmpeg's 128-sample stored state is treated as an optimized symmetric
  packing, not as evidence to change the standalone API state contract.
- **Boundary (pre-fix evidence only)**: N3A is a transform reference only. It does not claim native
  channel PCM integration or generic E-AC-3 coverage. Advanced syntax remains
  fail-closed, and no renderer/playback or DRC behavior is implied. The
  long-vector discrepancy must be resolved before treating the standalone
  transform as a production spectral-to-PCM acceptance layer. The long oracle
  and any PCM digest based on `yr[127-n]` for the first odd quadrant are
  invalid; use the Gate 8N N3A-FIX evidence above.

## Status refresh: 2026-08-26 (Gate 8N N2A3 supplied spectral matrix PASS)

- **Matrix scope**: `Eac3NativeCoefficientProbe` ran twice per bounded input
  prefix, with summary-only logs under `build-gate8a\\n2a3-matrix\\`. Config-3
  used `--max-frames 1000`; raw config-4 and all six
  `media\\gate8-dee-vectors\\eb3\\*.eb3` variants used `--max-frames 2000`
  (1000 E-AC-3 plus 1000 legacy companions). Every run exited 0 with
  `unsupportedFrames=0`, `bitEndEquality=PASS`, `coefficientFinite=YES`, and
  `ditherStateContinuity=PASS`.
- **Determinism**: Each pair's summary logs were byte-identical. The exact
  digest/hash/timing matrix is:

  | input | state digest | coefficient digest | log SHA-256 | wall time (runs) |
  |---|---|---|---|---|
  | config-3 | `53f00dcd23824826` | `d3889e4e0b15bf36` | `FEC2A41C7AE84D3E510AE9DBC09811C6965F81E9BA032F9336B2500BEC769255` | 0.78s / 0.81s |
  | raw config-4 | `52d91e8070d22688` | `8c06ffaf88fc1011` | `7129236D3631BBB7999ED33D1ECB71FFCB0DDE095F8E19705F1FF754158E60EF` | 1.31s / 1.35s |
  | DEE 1152k | `5d542015f9c50df0` | `d7da6cc4c6d78aab` | `0B3A279EE2ECFF7A54EC29F4FC43694A2AAF635837243BF5C84CE9B2A133919C` | 1.07s / 1.08s |
  | DEE 1280k | `8ec4ddd9694146b2` | `aea198be0e63d5ff` | `2F7B32BEC5ED1423A8E2CBB3E243762EB0946124EA96A953A07DC2D95242252B` | 1.10s / 1.09s |
  | DEE 1408k | `1c4ddf6bfb27b00a` | `fa539ce3a4c695ca` | `BC9C81704728190AEDD494DEDD729F2E3D51E22974549A923EDB973D22C8E406` | 1.12s / 1.14s |
  | DEE 1512k | `31d0594526d48b66` | `6dab8d65fa0839f0` | `8D39BC6405EFF8B1A31A31AE8594D1D7B901D3D5B5A9FB7E6F83F4563940C6F3` | 1.09s / 1.07s |
  | DEE 1536k | `09b5e0bed127f1e7` | `d7e73cd60366a3c9` | `65CBF48991C7231379954FC9B838B3C50325F199EBC968749F6309E16560717A` | 1.06s / 1.01s |
  | DEE 1664k | `62da16ff27edd999` | `5f0cd2efa2cb537a` | `F3D1C74B035F26F6244A32DE66DC50E8AF69DE93A349A32183CE905147E40A31` | 1.04s / 1.00s |
- **Performance/lifetime correction**: `MantissaBitReader` now holds a
  non-owning `const` byte view; transactional decode copies only pointer,
  byte-count, valid-bit, and cursor state instead of copying the full framed
  vector for every channel/block. The caller lifetime requirement is explicit
  in the header, and empty-reader selftests use the default constructor.
  Raw config-4 timing improved from 17.40s to 0.53s for 10 AU and from
  168.88s to 0.53s for 100 AU, with identical digest/acceptance results.
- **Acceptance boundary**: N2 is PASS/completed for the supplied
  ordinary-uncoupled
  spectral matrix only. It is not generic E-AC-3 coverage; SPX/AHT/coupling,
  enhanced coupling, GAQ, DBA, and delta-BA remain structured `Unsupported`.
  There is no IMDCT, PCM, DRC application, JOC, renderer, playback, CUDA, or
  SIMD implementation.

## Status refresh: 2026-08-26 (Gate 8N N2A2 production dither/oracle)

- **Implementation**: Reused the N1A audfrm/audblk cursor and the N2A1 shared
  mantissa primitive for ordinary uncoupled FBW/LFE coefficient vectors.
  Coefficient mode requires an explicit, resettable native
  `ReferenceDitherSource&`; an injected callback remains available for focused
  tests. The parser never resets a source per frame. Session mode records
  dither state start/end and generated sample counts across ordered channels,
  blocks, and frames. Inventory mode remains value-free.
- **TS boundary**: Local `docs/dev/Digital Audio Compression (AC-3, Enhanced
  AC-3) Standard.pdf`, TS 102 366 V1.4.1 §6.3.4 (PDF pages 66-67), requires
  reasonably random noise for `dithflag=1` and true zero for `dithflag=0`; it recommends uniform
  `[-1,+1]` scaled by about 0.707 (and identifies 0.75 or 0.5 as acceptable).
  It does not specify an LFSR, seed, exact sequence, or reset policy. The
  native xorshift64* stream is therefore an explicit implementation choice,
  not a claimed normative sequence.
- **Fail-closed boundary**: Reserved grouped composites remain structured
  `Unsupported`; SPX/AHT/coupling/enhanced coupling, GAQ, DBA, and delta-BA
  remain outside this slice. N0/sample evidence observes no active stereo
  rematrix branch, so no rematrix transform is applied. DRC remains presence-
  only (`drcApplied=NO`), with no IMDCT, PCM, JOC, renderer, playback, CUDA,
  SIMD, or FFmpeg/libav runtime path.
- **Focused evidence**: `Eac3NativeCoefficientProbe --self-test` passed 7
  cases: all BAP families, grouped continuation, LFE ownership, bap0
  dither-on/off, reserved structured failure, truncation context, and source
  reset/determinism. Existing Core (35), BSI (9), Exponents (10),
  BitAllocation (12), Mantissas (16), Audblk (9), and BlockState (10)
  self-tests also passed.
- **Real-sample evidence**: Two 10-AU config-3 runs on
  `build-gate8a\\n0-extract\\ipad-config3-1001.eac3` returned identical
  `stateDigest=dc4fbb229fdb8d4a` and
  `coefficientDigest=4953c34f4e0af23e`; both reported bit-end equality,
  finite coefficients, and dither-state continuity PASS. Two 10-AU E-AC-3
  config-4 runs (20 syncframes including legacy companions) on
  `media\\POWDER SNOW Live V9.8.6.eb3` returned identical
  `stateDigest=a7f6edb57c154fa7` and
  `coefficientDigest=6ef81ca6893d0a20`, with the same PASS results.
- **N0C preservation**: The adjacent N1A state probe still reports config-3
  frame offset `3072`, five FBW BAP vectors of length `217`, and block-1
  relative bit `5629` (`tmp\\n2a2\\final\\block-state-config3.log`).
- **Offline oracle**: Pinned FFmpeg `96f82f4fbbdc8f7525672bafbf37616ea5fd76ca`
  was instrumented only in a temporary ignored checkout and then restored.
  At the pre-IMDCT/pre-rematrix fixed-coefficient layer, isolated config-3
  frame byte offset 3072 matched 1092/1092 keys and BAP/exponents; all 682
  non-dither bins were within one Q23 unit. Dither sequence equality is not
  claimed; only range/statistical context was compared. Evidence is in
  `tmp\\n2a2\\final\\oracle-comparison.txt` and the paired logs.
- **Boundary**: N2A2 is a bounded spectral state/coefficient slice, not
  completion of N2. It is not a PCM/IMDCT decoder, JOC decoder, renderer,
  production playback path, or normative dither-sequence implementation.

## Status refresh: 2026-08-26 (Gate 8N N1A state-only snapshots)

- **Implementation**: Reused the bounded N0 `parseEac3Audblk()` cursor and
  added adjacent `native-eac3-block-state.{h,cpp}` validation/digest helpers.
  Accepted frames now expose absolute frame/audfrm/block boundaries and
  channel enclosing syntax-to-mantissa spans (not contiguous ownership slices),
  channel bandwidth/endMant, exponent new/reuse plus decoded exponent vectors,
  coarse/fine SNR reuse, fast-gain retention, bit-allocation parameters/reuse,
  complete BAP vectors and digests, rematrix flags, LFE identity, and one
  grouped-mantissa cursor ownership record per block. Rematrix snapshots retain
  effective flags and distinguish update from reuse/reset. Dynamic-range
  metadata is presence-only and `drcApplied=NO`.
- **Unsupported boundary**: SPX, AHT, coupling/enhanced coupling, GAQ, and
  active DBA remain structured `Unsupported`; this slice does not decode
  coefficient values, perform IMDCT, produce PCM, or touch JOC/renderer/
  playback paths.
- **Focused probe**: `Eac3NativeBlockStateProbe --self-test` passed 10 cases
  for new-to-reuse state, frame reset, channel isolation, LFE ownership,
  malformed context, and digest determinism. Config-3 (1000 AUs) reported five
  FBW BAP vectors of length 217 and block 1 relative bit 5629. Raw config-4
  (1000 AUs) reported 0 unsupported and 0 malformed frames; two 10-AU runs
  returned the same state digest.
- **Evidence commands**: The focused target was built in
  `build-gate8a/Release/Eac3NativeBlockStateProbe.exe`; local run output is
  under the command transcript for this worktree. Existing
  `Eac3NativeAudblkProbe --self-test` also passed after the state wiring.
- **Gate boundary**: N1A is a reusable syntax state machine only. It is not a
  native coefficient decoder, PCM/IMDCT path, JOC decoder, renderer, or
  production playback integration.

## Status refresh: 2026-08-26 (Gate 8N N0D feature inventory)

- **Implementation**: Added the FFmpeg-free bounded
  `native-eac3-audblk.{h,cpp}` parser and `Eac3NativeAudblkProbe` target. It
  follows TS 102 366 V1.4.1 Annex E.1.2.3/E.1.2.4 and ordinary clauses
  6.1-6.3 through the six-block cursor path. Reserved bap1/2/4 composite
  words are consumed at fixed 5/7-bit widths and reported as deterministic
  conformance warnings; no mantissa value is assigned. Dynamic-range words
  and BSI `compr`/`compr2` are presence-only; DRC is never applied. No PCM,
  IMDCT, renderer, or playback code changed.
- **Self-test**: `--self-test` passes 9 cases, including positive six-block
  reuse, grouped cursor continuation, LFE ownership, reserved bap1/bap4
  warning plus later syntax, and three tail truncation points.
- **Sample evidence**: The config-3 development extraction from
  `media/03. iPad.m4a` (`build-gate8a\n0-extract\ipad-config3-1001.eac3`)
  reports 1000 E-AC-3 frames, 6000 blocks, 1000 accepted, 0 unsupported, and
  0 malformed. It observes 18 block-switch events, 29964 dither-on and 36
  dither-off instances, 6000 audblk dynrng words, `compr` in 1000 BSI records,
  and no `compr2`/`dynrng2`; bamode syntax occurs in 653 frames/3918 blocks.
  The raw config-4 EB3 and all six DEE EB3 variants each report 2000
  syncframes (1000 legacy plus 1000 E-AC-3), 1000 accepted E-AC-3 frames,
  0 unsupported, 0 malformed, and 6000 blocks. Each has 24000 dither-on
  instances, `compr` in 1000 BSI records, no `compr2`/`dynrng2`, and bamode
  syntax in 283 frames/1698 blocks. All advanced-tool counters are zero.
  The complete deterministic summaries and SHA-256 pairs are in
  `build-gate8a\n0d-matrix\` and `docs/dev/eac3-native-feature-inventory.md`.
- **N0A/N0C state/oracle boundary**: Against reviewed local FFmpeg source
  `build-mm/ffmpeg-src` commit
  `96f82f4fbbdc8f7525672bafbf37616ea5fd76ca`, SNR strategy-2/3 reuse remains
  per-channel, later `fgaincode=0` retains fast-gain state, LFE uses current
  coarse SNR, and FBW/LFE share one block-level grouped cursor. The former
  config-3 frame-1 BAP divergence was proven to be the critical-band-45
  width table and corrected; all five FBW channel BAP sequences now match,
  and block 1 begins at relative bit 5629 in both traces. FFmpeg's full
  composite lookup behavior supports fixed-width interoperability only; it is
  not linked or copied into the target.
- **Gate result**: N0 inventory is **PASS** for the bounded real-sample
  matrix. This is syntax inventory only: no native coefficient-value
  decoder, PCM, IMDCT, JOC, renderer, production demux, or DRC application is
  claimed. DRC metadata is counted and remains off. Future N1-N3 work must
  stop at the first active SPX/AHT/coupling/enhanced-coupling/GAQ/DBA branch
  and return structured `Unsupported`.

## Status refresh: 2026-08-26 (Gate 8 execution order)

- **Roadmap boundary**: Added
  `docs/dev/eac3-joc-next-roadmap.md` as the active dependency order from the
  native decoder through the self-rendered headphone output. It does not claim
  new decoder, renderer, playback, or listening evidence.
- **Next hard gate**: Inventory the actual `audfrm/audblk` coding tools used by
  both config-3 and config-4 sample families before ordering
  DBA/coupling/SPX/AHT/GAQ, coefficient reconstruction, and IMDCT work. This
  prevents the decoder order from being chosen from synthetic coverage alone.
- **Integration order**: Raw EB3 is the first FFmpeg-free offline audible
  route. Stereo WASAPI follows a validated offline decoder/JOC/SOFA chain;
  native M4A packet, trim, and seek handling remains a separate later gate.
  DRC application stays off. CUDA and SIMD are not correctness dependencies;
  CPU optimization follows measured profiling.
- **Execution contract**: Assign one numbered slice to Luna at a time with
  disjoint file ownership and explicit stop conditions. The immediate slice is
  N0 feature inventory only; no production playback, IMDCT, renderer tuning,
  or acceleration belongs in that handoff.

## Status refresh: 2026-08-25 (self-rendered headphone target status)

- **Implementation boundary**: Gate 8A-1, Gate 8N-1a/1b, Gate 8N-2a,
  Gate 8N-2b-1, Gate 8B-1/2, and Gate 8C-1/2/3a/3b are implemented with
  standalone diagnostics. Gate 8N-2b-1 reaches bounded native bit allocation
  for one uncoupled FBW channel, and Gate 8N-2b-3 reaches a bounded native
  mantissa/dequantization primitive that is not yet audblk-connected; Gate
  8C-2 reaches the single-triplet
  BS.2127 §6.1.1 gain primitive; Gate 8C-3a reaches nominal System H
  supporting-facet topology plus the lower VirtualNgon primitive. The full
  chain remains incomplete: DBA/coupling-domain allocation, audblk integration
  around the mantissa primitive, IMDCT, Quad/extent,
  decoder-scene wiring, and SOFA/BRIR/WASAPI integration remain outstanding.
- **Route boundary**: The primary target remains self-rendered headphones:
  BS2127/BS2051 virtual speakers -> SOFA/BRIR -> stereo WASAPI with system
  Spatial Sound disabled. Windows Spatial Audio remains an optional,
  non-normative comparison bridge.
- **Next sequence**: Complete DBA/coupling-domain allocation and audblk
  integration/IMDCT and decoder-scene wiring; then implement Quad/extent and
  Gate 8D SOFA/BRIR/WASAPI integration. Detailed plan:
  `docs/dev/eac3-joc-full-chain-plan.md`.
- **Project validation**: `scripts\validate-all.ps1 -BuildDir build-gate8a
  -Configuration Release -SkipSmoke` passed all 10 unit-test suites and the
  report-schema self-test; smoke was explicitly skipped. Aggregate report:
  `build-gate8a/validation-report.json`.

## Status refresh: 2026-08-25 (Gate 8N-1a native syntax parser)

- **Implementation**: Added the FFmpeg-free
  tools/atmos-joc-probe/native-eac3-core.h/.cpp parser and
  native-eac3-core-probe.cpp. It has a bounded bitreader, E-AC-3
  syncframe/header subset (stream type, substream ID, frame size, sample rate,
  block count, bsid, acmod, LFE), legacy AC-3 inventory distinct from
  E-AC-3 strmtyp=2, dependent/additional
  markers, and six-block/1,536-sample access-unit assembly with structured
  disposition, failure stage, flow, and reason results. The assembler also
  has reset, cancel, and one-shot flush guards. chanmap, mix metadata,
  audfrm/audblk, and CRC contents remain unparsed.
- **CRC boundary**: This gate checks frame bounds only. It reports
  frameBoundaryRangeChecked=YES, crcRangeChecked=NO, and crcVerified=NO;
  no CRC polynomial is calculated.
- **Self-test/build**: The new Eac3NativeCoreProbe target links no FFmpeg
  library and passed --self-test with 34 cases covering 1/2/3/6-block
  headers, six-block grouping, parent-local dependent/additional topology
  (five topology keys, each reaching six blocks), truncation,
  reserved stream/rate (including fscod=3 and bsid 9/10/17), invalid block
  shape, sample-rate change, overflow, orphan/non-immediate dependent,
  dependent shape/rate, additional-ID gaps, legal strmtyp=2 standalone AUs
  with strmtyp=2-dependent rejection,
  base-boundary sequencing, bitreader overrun, reset/flush/cancel, and the
  CRC capability boundary.
  Gate 4, Gate 5C, and Gate 6C self-tests also passed with the existing
  docs/dev/ts_103420_tables.c.
- **Raw-sample differential**: On
  media\POWDER SNOW Live V9.8.6.eb3, the native probe's --max-units 1000
  prefix reported 2,000 syncframes, 1,000 access units, 6,656,000 framed
  bytes, 1,536,000 timeline samples, 1,000 legacy AC-3 and 1,000 dependent
  frames at 48 kHz. These match the legacy Gate 1 prefix (including 1,000
  complete units and 1,536 samples per unit). This is syntax/topology
  differential evidence only, not decoded PCM.
- **Evidence boundary**: This is Gate 8N-1a, not Gate 8N completion. The new
  probe accepts contiguous raw E-AC-3/EB3 only; it does not demux the
  media\03. iPad.m4a container, and the existing access-unit monolith has not
  been rewired to call the new parser. No exponents, bit allocation,
  mantissas, coupling, SPX, AHT, IMDCT, native PCM, JOC decode, production
  playback, or headphone listening evidence is claimed.

## Status refresh: 2026-08-25 (Gate 8N-1b bounded E-AC-3 BSI)

- **Implementation boundary**: Added the FFmpeg-free
  `native-eac3-bsi.{h,cpp}` parser and `Eac3NativeBsiProbe`. It consumes
  ETSI TS 102 366 V1.4.1 E.1.2.2 `bsi()` through the `audfrm()` boundary,
  bounded by `FrameHeader::endBit`. It parses the conditional dialnorm/
  compression, dual-mono, dependent `chanmape/chanmap` (Table E.1.4 weighted
  channel count), mixing metadata including mixdef 1/2/3 lengths, pan/frame
  mix flags, informational metadata, convsync, Type 2 blkid/frmsizecod, and
  addbsi length/payload bounds. It does not parse audfrm/audblk or coefficients.
- **Legacy and capability boundary**: Legacy AC-3 remains inventory-only and
  returns `bsiParsed=NO`. CRC remains unverified. A dependent `chanmap` whose
  Table E.1.4 weighted count does not equal the coded channel count is rejected.
- **Validation**: `Eac3NativeBsiProbe --self-test` passed 9 cases, including
  the config-4 contract (`bsiEndBit=101` frame-relative, `dialnorm=22`,
  `compr=255`, `chanmap=0xA010`, weighted 4 channels, `addbsil=1`), weighted
  chanmap rejection, mixdef=3 cursor alignment with both 3-bit and zero fill,
  Type 2 six-block implicit `blkid=1`, and legacy `bsiParsed=NO`. On
  `media\POWDER SNOW Live V9.8.6.eb3 --max-units 1000`, the probe passed
  exactly 2,000 frames, 1,000 access units, 1,000 dependent BSI parses,
  1,000 legacy inventory frames, and `chanmapA010=1000`; completion-boundary
  lookahead is excluded and no incomplete pending AU is flushed after the
  bounded stop. No native decoded PCM or production playback claim belongs
  to Gate 8N-1b.

## Status refresh: 2026-08-25 (Gate 8N-2a native exponent primitives)

- **Implementation boundary**: Added the dependency-free
  `native-eac3-exponents.{h,cpp}` primitive and
  `Eac3NativeExponentsProbe`. It decodes the Clause 6.1 grouped 7-bit base-5
  codes (M1/M2/M3, delta M-2), D15/D25/D45 expansion, transmitted ordinary/LFE
  absolute exponents 0..15 with expanded values bounded at 0..24, and the
  fixed seven-exponent LFE D15 helper. Coupling exposes `cplabsexp<<1` as an
  explicit reference exponent while returning only the requested actual
  coupling-bin count; its reuse state validates block, prior, and start/end
  mantissa coordinates independently of ordinary bandwidth state. Partial
  D25/D45 tail groups stop at the requested count. It does not parse or
  implement bit allocation, mantissas, coupling coordinates, or IMDCT.
- **Reuse contract**: Reuse requires a valid prior-block state, matching
  bandwidth, matching coefficient count, and a non-zero block index; block-0
  reuse, missing prior state, bandwidth changes, and extra group codes reject.
- **Validation**: `Eac3NativeExponentsProbe --self-test` passed 10 cases,
  covering grouped code 0/124, code 125 rejection, D15/D25/D45 including
  partial tails, the gexp=86 oracle, transmitted absolute range and
  underflow/overflow, coupling reference/bin-count and coupling-specific
  reuse rejection, LFE seven exponents, and ordinary reuse state. Gate 8N-1a
  and 8N-1b regression probes passed;
  `scripts\validate-all.ps1 -BuildDir build-gate8a -Configuration Release
  -SkipSmoke` passed all 10 unit-test suites and the report-schema self-test;
  smoke was explicitly skipped.
  No native PCM, JOC, production playback, or listening evidence is claimed
  by Gate 8N-2a.

## Status refresh: 2026-08-25 (Gate 8N-2b-1 native FBW bit allocation)

- **Implementation boundary**: Added the dependency-free
  `native-eac3-bit-allocation.{h,cpp}` primitive and
  `Eac3NativeBitAllocationProbe`. It implements the TS 102 366 V1.4.1
  clause 6.2.2 initialization, exponent-to-PSD mapping, Table 6.12 band
  integration, lowcomp/excitation, Table 6.15 masking, and Table 6.16 bap
  selection for one uncoupled FBW channel with ordinary exponents. The owned
  request validates `fscod=0..2`, mantissa/exponent ranges, and all allocation
  code widths. The result exposes `psd`, `bndpsd`, `excite`, final rounded
  `mask`, raw `hearingThreshold`, `bap`, `snroffset`, `lowcomp`, and the
  frame-level zero-SNR fast-path flag. The raw threshold vector is indexed by
  band; zero-SNR leaves its preallocated entries at zero.
  `allActiveSnrOffsetsZero` is caller-owned: local channel offsets of zero
  with that flag false still run the calculation; an asserted global
  shortcut must also have local zero offsets.
- **Unsupported boundary**: Reduced sample rate (`fscod=3`), coupled FBW,
  AHT, SPX, enhanced coupling, GAQ, and resolved DBA modes other than
  `DbaMode::None` are structured `UNSUPPORTED`; `deltbae` raw coding remains
  parser-owned (`00` reuse, `01` new, `10` none, `11` reserved). No audblk
  parser, mantissa reader, or production worker is connected. This is not a
  complete E-AC-3 decoder or PCM claim.
- **Table/oracle scope**: Table 6.15 is stored in explicit `[band][fscod]`
  order after correcting the PDF's two-column presentation. The self-test
  pins the fs0/band1 `0x4d0`, fs1/band0 `0x4f0`, and fs2/band49 `0x4e0`
  entries directly through the result's raw threshold vector, while retaining
  quantized mask checks; it also checks the non-constant band 21→22→23 leak
  transition plus a fixed BAP fragment. Band 22 continues the leak state
  established over bands 0..21; it is not re-seeded from `bndpsd[22]`.
- **Validation**: Built with
  `cmake --build build-gate8a --target Eac3NativeBitAllocationProbe
  --config Release -- /m:1`; `Eac3NativeBitAllocationProbe --self-test`
  passed 11 synthetic cases covering the frame/global zero-SNR contract,
  fscod 0/1/2, Table 6.15 ordering, constant exponents and band integration,
  band-22 leak continuity, lowcomp boundary-sized ranges, BAP address
  clamping/fixed fragments, malformed inputs, and unsupported advanced/DBA
  tools. The combined worktree then passed all 10 unit-test suites and the
  report-schema self-test with smoke explicitly skipped. No real-sample
  bit-allocation differential or listening evidence is claimed by this gate.

## Status refresh: 2026-08-25 (Gate 8N-2b-3 native mantissa primitive)

- **Implementation boundary**: Added the dependency-free
  `native-eac3-mantissas.{h,cpp}` primitive and
  `Eac3NativeMantissasProbe`. It uses a bounded MSB-first bit reader,
  transactional cursor/state updates, Table 6.17 mantissa widths, Tables
  6.19–6.23 symmetric quantization, and asymmetric two's-complement
  dequantization for bap 6–15. The output is double precision: normalized
  mantissa ratio plus `mantissa * 2^-exponent` transform coefficient.
- **Grouped-state boundary**: bap 1/2/4 use 5/3, 7/3, and 7/2 grouped
  codes. The explicit cache may cross ordered channel calls within one audio
  block, retaining independent pending slots for each grouped BAP; block
  changes with pending groups are rejected, and block-end pending values are
  discarded as specification dummies. Truncation and invalid group/scalar
  codes commit neither output, bit cursor, cache state, nor absolute
  coefficient cursor.
- **Dither cursor**: `MantissaDecodeState::absoluteCoefficientCursor` advances
  across calls for every coefficient, including non-dithered baps, and is
  reset at block end or explicit reset. `DitherSource` receives that absolute
  within-block index; failed transactions do not advance it.
- **Dither boundary**: bap 0 uses an injected `DitherSource`; `dithflag=0`
  produces exact zero. `dithflag=1` requires a finite source value in
  [-1,+1] and is tested for repeatability and exponent scaling. No normative
  LFSR is implemented or claimed; TS 102 366 permits a reasonable random
  sequence and scaling tolerance.
- **Validation**: The self-test covers scalar symmetry/extremes, grouped
  boundary codes (bap1 0/26, bap2 0/124, bap4 0/120), invalid bap2/4 codes,
  full legal bap3/bap5 code golden sets, invalid bap3/bap5 codes,
  degrouping and scalar bap 5–15 widths, same-channel/block cache
  continuation, absolute dither cursor continuity, reset/context rejection,
  block-end dummy discard, truncation/invalid-code transactionality, and
  injected dither. The 16-case self-test passes. This gate does not connect
  audblk, bit allocation, coupling, or IMDCT and makes no PCM or
  production-playback claim.

## Status refresh: 2026-08-25 (local DEE coverage vectors)

- **Local vectors**: Dolby Encoding Engine `5.2.1-5994839` encoded the Logic
  Pro ADM BWF master into six five-second Atmos E-AC-3/JOC vectors at
  384/448/576/640/768/1024 kbps and three ordinary 5.1 E-AC-3 vectors at
  192/448/640 kbps. Media, XML, XSD, and logs remain ignored under
  `media/gate8-dee-vectors/`; only the hashes and evidence manifest are
  tracked in `docs/dev/eac3-dee-test-vectors.md`.
- **Syntax evidence**: The first 100 AUs of every Atmos vector contain EMDF
  payloads 11 and 14 and pass the bounded JOC syntax/math checks. The 384-kbps
  encode reports config 3 with 11 objects; the other five report config 3
  with 15 objects. Their lack of dependent frames is not a no-JOC result.
- **Boundary**: DEE CLI/XSD exposes Atmos E-AC-3 output but no separate
  online/Blu-ray profile selector. Its ordinary `ddp71` route rejects the ADM
  Atmos input, so no guessed "Blu-ray Atmos" vector was manufactured. These
  are non-normative differential/coverage inputs, not native decoder, PCM,
  conformance, runtime-dependency, or listening evidence.
- **DEE Media Encoder Blu-ray-profile EB3 coverage**: The ignored local
  `media/gate8-dee-vectors/eb3/` set now contains six hashes for nominal
  1,152/1,280/1,408/1,512/1,536/1,664 kbps files (43,350,000 through
  62,550,000 bytes), with the full table in
  `docs/dev/eac3-dee-test-vectors.md`. Despite the `5s` filenames, each is
  9,375 AUs / 18,750 frames / 300 seconds. Each AU has a 16-byte wrapper,
  legacy AC-3, and dependent E-AC-3; the first 100 AUs have 200 frames, 100
  dependent frames, BSI `chanmap=0xA010`, EMDF 11/14 each 100, config 4, and
  15 objects. JOC syntax/math report `total=100, covered=100`, while semantic
  phase reports `total=100, covered=0, unsupported` in the base `--joc`
  command. Existing Gate 6C/7 config-4 handling remains valid when those
  later gates are requested; this base counter is not a new config-4 blocker.
  Wrapper/first-AU byte
  invariants and the legacy raw EB3 first-100 wrapper-0 regression are
  recorded in the manifest; media remains
  ignored and runtime DRC is `off`.
- **Native framing evidence**: The native core and BSI probes strictly remove
  only the observed offset-zero 16-byte wrapper and advance by declared
  AC-3/E-AC-3 frame lengths; they do not scan compressed payload for a guessed
  syncword. Across all six files, `--max-units 100` reports
  `eb3WrapperCountTotal=9375`, `eb3WrapperCountCovered=100`, 200 frames, 100
  AUs, 100 dependent frames, and PASS. The shared AU assembler now commits
  parent/SID/topology state transactionally, and the 35-case native-core
  self-test covers failed-frame rollback, wrapper field failures, opaque
  carriage metadata, payload pseudo-sync, and raw trailing garbage. The
  native Core/BSI/mantissa/bit-allocation/VirtualNgon executables have no
  libav/FFmpeg PE imports; `Eac3AccessUnitProbe` remains a development-only
  FFmpeg differential oracle.

## Status refresh: 2026-08-25 (Gate 8B-1 scene contract)

- **Implementation**: Added the dependency-free
  `tools/atmos-render/scene-model.h/.cpp` and
  `scene-model-probe.cpp` foundation for `RenderScene`, `RenderElement`,
  `MetadataTimeline`, fixed bed labels, object IDs/timestamps, identity
  `ListenerPose`, and generation reset. Generation zero is an explicitly
  invalid, non-throwing state; operations reject it until a valid reset.
  Metadata timestamps must fall within the corresponding object's half-open
  `[start, start + duration)` range. Snapshots use the classic locale and
  fixed float precision.
- **Self-test/build**: The post-repair `Eac3SceneModelProbe` run passed 30
  scene contract cases, including generation-zero rejection/recovery,
  half-open metadata timestamp ranges, finite-value validation, deterministic
  snapshots, and stale-generation reset behavior.
- **Evidence limit**: This is only the Gate 8B-1 scene contract. It does not
  implement `SpeakerLayout`, `GainInterpolator`, a panner, SOFA/BRIR,
  convolution, a sink, or stereo PCM output. No decoder, production playback,
  WASAPI, or headphone listening behavior changed.

## Status refresh: 2026-08-25 (Gate 8B-2 stereo diagnostic panner)

- **Implementation**: Added the pure C++
  `tools/atmos-render/stereo-diagnostic.h/.cpp` and
  `stereo-diagnostic-probe.cpp` slice. It provides a fixed two-speaker
  diagnostic layout, explicit bed-label routes, Cartesian x/z to azimuth
  conversion, equal-power L/R panning, sample-accurate linear gain ramps,
  and reset-cleared ramp state. LFE is disabled explicitly; no arbitrary
  attenuation is applied.
- **Build/self-test**: Built and ran with
  `cmake --build build-gate8a --target Eac3SceneModelProbe Eac3StereoDiagnosticProbe --config Release -- /m:1`.
  The stereo diagnostic probe passed 24 cases, covering hard-left/center/
  hard-right, symmetric power, block-size invariance, reset, NaN rejection,
  LFE disabled, finite clip counting, and overflow counting.
- **Evidence limit**: The panner retains finite samples above unity and only
  counts clipping/overflow; it has no DRC, limiter, or automatic gain. This
  slice does not provide PCM file output, a PCM/WASAPI sink, a formal BS.2051
  layout, SOFA/BRIR, convolution, decoder integration, or headphone listening
  evidence.

## Status refresh: 2026-08-25 (Gate 8C-1 BS.2051 System H layout)

- **Implementation**: Added the dependency-free
  `tools/atmos-render/bs2051-layout.h/.cpp` and
  `bs2051-layout-probe.cpp` contract for the fixed BS.2051 System H catalog:
  22 speakers in the `9+10+3` upper/middle/bottom arrangement. It provides
  label-to-position lookup, nominal azimuth/elevation, unit-vector conversion,
  uniqueness, layer-count, finite-range, normalization, and mirror checks.
  LFE1/LFE2 are explicitly `separate` and are not part of the 22-point
  convolution layout.
- **Build/self-test**: The local `Eac3Bs2051SystemHProbe` run passed 12
  cases. It compares the exported local BBC `bbcrdlr_systemH.sofa` emitter
  order and positions without opening SOFA/HDF5 at runtime. The BS.2051
  nominal upper elevation is 30 degrees, while the BBC SOFA measurements are
  mostly 40 degrees; strict comparison reports a maximum 10-degree elevation
  difference, retained as a measurement discrepancy rather than silently
  normalized.
- **Evidence limit**: Production code has no SOFA/HDF5 runtime dependency and
  this gate does not implement a BS.2127 panner, virtual-speaker gain math,
  convolution, BRIR loading, PCM output, or a sink. The catalog is a layout
  contract only, not formal binaural or Dolby-equivalence evidence.

## Status refresh: 2026-08-25 (Gate 8C-2 BS.2127 triplet primitive)

- **Implementation**: Added the dependency-free
  `tools/atmos-render/bs2127-triplet.h/.cpp` and
  `bs2127-triplet-probe.cpp` slice for the single-triplet gain solve primitive
  corresponding to BS.2127 §6.1.1. Three speaker unit vectors form the matrix
  columns; the primitive reports raw gains, determinant, an infinity-norm
  condition estimate, and an explicit rejection reason. Singular or
  near-singular determinants reject. Meaningful negative gains reject as
  outside the triplet; only configured round-off-sized negatives are zeroed
  and counted before power normalization.
- **Build/self-test**: Built and ran with
  `cmake --build build-gate8a --target Eac3Bs2127TripletProbe --config Release -- /m:1`.
  `Eac3Bs2127TripletProbe.exe` passed 13 cases covering axis/interior/boundary
  points, power normalization, negative-gain rejection and tolerance,
  singular/NaN inputs, speaker-order permutation, condition/determinant
  reporting, and unit-vector tolerance.
- **Evidence limit**: This is only a single-triplet mathematical primitive. It
  does not select triplets, implement VirtualNgon/extent, perform complete
  BS.2127 rendering, produce PCM, load SOFA/BRIR data, convolve, track a head,
  or provide a sink, binaural, or Dolby-equivalence claim.

## Status refresh: 2026-08-25 (Gate 8C-3a System H nominal topology)

- **Implementation**: Replaced the incorrect all-combination selector with the
  dependency-free `tools/atmos-render/bs2127-selector.h/.cpp` and
  `bs2127-selector-probe.cpp` topology slice. It enumerates supporting planes
  over the 22 System H nominal points plus the BS.2127-required lower virtual
  point `(0,0,-1)`, merges coplanar plane duplicates within explicit tolerance,
  and stably classifies raw hull facets as `Triplet`, `Quad`,
  `VirtualHullFacet`, or `UnsupportedNgon`. The upper virtual point is not
  added because System H has `T+000`.
- **Topology evidence**: For the current System H coordinates and current
  probe tolerances only (not normative fixed counts), the local probe reports
  23 points and 38 unique facets: 26 Triplet, 4 Quad, 8
  `VirtualHullFacet`, 0 UnsupportedNgon; 50 raw supporting-plane hits were
  merged to 38 facets with 12 duplicate-plane merges. Every retained facet is
  checked as a supporting plane with stable, sorted real indices and no
  duplicate vertex sets. The 8 lower-pole raw facets are not 8 VirtualNgon
  regions; a future slice must aggregate them into one lower-pole VirtualNgon
  and restore its ring adjacency.
- **Build/self-test**: Built and ran with
  `cmake --build build-gate8a --target Eac3Bs2127SelectorProbe --config Release -- /m:1`.
  `Eac3Bs2127SelectorProbe.exe` passed 62 cases covering topology order,
  supporting-plane and duplicate checks, virtual-point rules, facet-kind and
  independent determinant/condition checks, all 22 identity-guard shortcuts,
  identity-guard-disabled behavior, every real Triplet facet's interior
  direction through selector and solver, boundary/overlap repeatability, and
  NaN handling.
- **Evidence limit**: Runtime currently calls the BS.2127 triplet primitive
  only for real three-vertex facets in topology order, with determinant and
  condition checked by an independent geometry helper. Quad and
  VirtualHullFacet facets return structured `Unsupported`; no arbitrary
  triangulation, lower-pole VirtualNgon aggregation/downmix,
  actual-vs-nominal position adaptation, full renderer, PCM, SOFA/BRIR,
  WASAPI, or Dolby-equivalence claim is made.

## Status refresh: 2026-08-25 (Gate 8C-3b lower VirtualNgon)

- **Implementation**: Added the dependency-free
  `tools/atmos-render/bs2127-virtual-ngon.h/.cpp` and probe. It aggregates
  the current lower virtual raw hull facets by real-speaker edge adjacency
  into one closed `VirtualNgon` ring, emits one virtual-plus-edge triangle per
  ring edge in that recovered adjacency. The implementation uses a fixed
  deterministic project-local System H ring direction for stable calls; that
  direction is not a normative BS.2127 ordering constant. It applies the
  BS.2127 §6.1.2.2 first-valid triangle rule. For the direct downmix specified
  in §6.1.3.1,
  `Wdmx=1/sqrt(n)` is applied to the virtual gain over the n adjacent real
  speakers, followed by a second power normalization. Output is exactly 22
  real gains; the virtual gain is not exposed as an output channel. The
  render-time validator is intentionally fixed to the System H primitive:
  exactly 8 ring vertices/triangles and complete coverage of all 8 lower
  virtual facets are required; ring rotation/reversal is accepted, while the
  production builder uses one deterministic start/direction.
- **Scope boundary**: Quad regions remain structured `Unsupported`; no
  arbitrary quad triangulation, private Cavern/EAR weighting, extent,
  actual-vs-nominal correction, SOFA/BRIR convolution, PCM, or WASAPI sink is
  introduced by this slice.
- **Build/self-test**: Built and ran with
  `cmake --build build-gate8a --target Eac3Bs2127VirtualNgonProbe --config
  Release -- /m:1`; `Eac3Bs2127VirtualNgonProbe.exe` passed 17 cases. The
  probe covers the deterministic local ring direction and closure,
  every-ring-edge first-valid selection, exact two-speaker edge boundary
  gains, equal-power and left/right-symmetric downmix, finite 22-channel
  output, final power one, invalid/tampered/incomplete-region input, and
  structured unsupported behavior. Ring/count values are current-coordinate
  regression evidence, not normative BS.2127 constants.

## Status refresh: 2026-08-25 (Gate 8A-1 injected decoder-session seam)

- **Implementation**: Added `tools/atmos-joc-probe/joc-session.h/.cpp` with
  the injectable `ICoreDecoder` contract (`Joc`/`NotJoc`/`Unsupported`/
  `Malformed`, native layout/sample count/timestamp/reset/flush/cancel and
  structured reasons). `Eac3AccessUnitProbe` now calls the same Session for
  Gate 5B -> QMF -> Gate 6C state orchestration. Added the independent
  `Eac3JocSessionProbe` fake-core/session self-test target.
- **Build/self-test**: Configured `build-gate8a` against the existing
  self-built FFmpeg runtime, then built
  `cmake --build build-gate8a --target Eac3JocSessionProbe Eac3AccessUnitProbe --config Release -- /m:1`.
  `Eac3JocSessionProbe.exe docs/dev/ts_103420_tables.c` passed 23 cases
  (core 13, session 9, Gate 6C baseline 20). The session cases cover valid
  config-3/config-4 Gate 5B -> QMF -> Gate 6C frames, reset equivalence,
  malformed/unsupported transactional rejection, callback rejection/cancel,
  and per-generation flush guards. `FlowStatus` is separate from content
  disposition, and `FailureStage` separates Validation/Math/Sequence/QMF/
  Gate6C failures without parsing reason text; diagnostic callback
  rejection/cancel now stops the run.
  `Eac3JocQmfProbe --table
  docs/dev/ts_103420_tables.c`, `Eac3JocGate6cProbe`, the existing access-unit,
  Gate 5A and Gate 5B self-tests, and
  `scripts\run-tests.ps1 -BuildDir build-gate8a -Configuration Release` all
  passed; the project test report is `build-gate8a\test-report.json`.
- **1000-unit evidence**: `media\03. iPad.m4a` (config 3) and
  `media\POWDER SNOW Live V9.8.6.eb3` (config 4) both passed Gate 6C with
  1000 associated/reconstructed units and zero fallback; output samples were
  respectively 1,533,808 and 1,536,000. Gate 5C/QMF and access-unit results
  remained PASS.
- **Evidence limit**: This is Gate 8A-1 only. The fake adapter is the current
  contract self-test; no libav packet decoder adapter has been claimed, no
  complete native TS 102 366 decoder exists, and no production playback/UI,
  Windows worker, or headphone listening path was changed or verified.

## Status refresh: 2026-08-25 (user A/B decision pending iPad sample)

- **User listening feedback**: The user currently prefers the DRC0 Atmos/JOC
  trial and considers disabling stream DRC plausible. The NOISE1 difference was
  inconclusive by ear.
- **Recommendation**: Keep `cons_noisegen=0` for now. Treat `drc_scale=0` as
  the leading default candidate for the Atmos/JOC object path only; do not
  change ordinary E-AC-3 playback globally until the spatially more revealing
  `media\03. iPad.m4a` A/B is completed.
- **Next acceptance gate**: Run the same baseline/DRC0/NOISE1 comparison on
  the iPad M4A, preserving its container priming metadata and the existing
  renderer geometry. Record whether the larger spatial difference is upstream
  PCM, DRC, noise-seed exactness, or the final renderer before changing the
  default.

## Status refresh: 2026-08-25 (E-AC-3 decode-option A/B playback)

- **Implementation**: `Eac3AccessUnitProbe` now accepts
  `--eac3-drc-scale 0..6` and `--eac3-cons-noisegen 0|1`. The selected values
  are passed through `avcodec_open2` to both the trim-check and coded-domain
  libav decode instances, and are printed in the run evidence. Defaults remain
  `drc_scale=1` and `cons_noisegen=0`, preserving the previous behavior.
- **Short controlled A/B**: Baseline, DRC0, NOISE1, and DRC0+NOISE1 each ran
  the same 100-unit Gate 7C path. Every case passed Gate 6C, Gate 7B, native
  `5.1.2`/`fltp` pairing, 320 render commits, 153,600 submitted frames,
  zero underrun, and metric consistency. NOISE1 changed object peaks only at
  the small floating-point level; DRC0 did not change the maximum-peak metric,
  so peak equality is not treated as an audibility conclusion.
- **Full endpoint trials**: DRC0 (`--eac3-drc-scale 0`) and NOISE1
  (`--eac3-cons-noisegen 1`) were each run for 1,000 units on
  `media\POWDER SNOW Live V9.8.6.eb3`. Both completed with 3,200 render
  commits, 1,536,000 submitted frames, zero underrun, and
  `gate7cMetricConsistency=PASS`. These are endpoint-submission PASS results;
  subjective transparency remains pending the user's A/B listening report.
- **Manual commands** (run from the repository root):
  `build-luna-gate7c2\Release\Eac3AccessUnitProbe.exe
  "media\POWDER SNOW Live V9.8.6.eb3" --max-units 1000 --joc-gate7c
  --eac3-drc-scale 0` for DRC0, or replace the final option with
  `--eac3-cons-noisegen 1` for NOISE1. Keep all other renderer options
  unchanged during comparison.
- **Validation**: `scripts\run-tests.ps1 -NoBuild` passed all 10 suites; the
  option implementation was compiled into
  `build-luna-gate7c2\Release\Eac3AccessUnitProbe.exe`.
- **Next decision**: If DRC0 is audibly more transparent, make full-range DRC
  the JOC-path default while retaining an override. If NOISE1 is not audible,
  leave it off; it is primarily an exactness/reproducibility control, not a
  room-rendering feature.

## Status refresh: 2026-08-25 (FFmpeg E-AC-3 base-decoder DRC audit)

- **Boundary**: FFmpeg's demux/container reading does not determine the
  binaural room or object positions. However, Gate 6 obtains the eight-channel
  E-AC-3 base PCM from libavcodec before the custom JOC/OAMD reconstruction, so
  decoder-side gain processing can affect every reconstructed object.
- **Current behavior**: `decodeNativePcmForPairing()` opens the E-AC-3 decoder
  without setting private decoder options. The installed FFmpeg decoder reports
  `drc_scale=1` by default, `heavy_compr=false`, `target_level=0`, and no forced
  downmix. The supplied DME stream remains native 48 kHz planar float,
  eight-channel `5.1.2`; there is no observed layout conversion or stereo
  downmix in this path.
- **File-specific evidence**: Decoding the first 32 seconds of
  `media\POWDER SNOW Live V9.8.6.eb3` to float PCM produced different hashes:
  `drc_scale=1` ->
  `13a622b71446b432e5d01d9e134e4c38144a9096e2a89d2bafe3364a70acb1c5`,
  `drc_scale=0` ->
  `dadbccabcf0a9426c6bed4f1aee3f3f74798dd68e7d1b76412be14f7a8239409`.
  Therefore this stream contains effective dynamic-range metadata and the
  current default changes upstream PCM; the difference is not hypothetical.
- **Conclusion / next gate**: Do not start a replacement E-AC-3 core decoder.
  First add a narrow, logged `drc_scale=0|1` A/B to both libav decode paths and
  repeat the same 1,000-unit Spatial Audio run. If full-range decoding improves
  transparency, retain libavcodec and make the JOC/object path's DRC policy
  explicit. Only investigate a new base decoder if controlled PCM comparisons
  later prove a libavcodec conformance defect that cannot be disabled or fixed.
- **Other option audit**: The actual in-process runtime is the self-built
  FFmpeg `git-2026-04-16-96f82f4` audio core, and reports the same defaults as
  the host comparison build. `heavy_compr` is false, `target_level=0` leaves
  dialnorm target normalization unapplied, and no `downmix` layout is supplied.
  Enabling heavy compression or a non-zero target changes this file's PCM, but
  neither is active in the current path.
- **Secondary exactness variable**: `cons_noisegen` is false by default. FFmpeg
  still performs stream-directed zero-bit-mantissa dithering; this option only
  reseeds its generator from each compressed frame for reproducible noise.
  Enabling it also changed the 32-second PCM hash. This is unlikely by itself
  to explain a large room-width difference, but is worth a second controlled
  A/B because JOC reconstruction consumes the decoded base PCM rather than
  treating it only as final channel audio.
- **Further exclusions**: The native float-planar frames are copied directly;
  there is no `libswresample`, sample-format conversion, limiter, or decoder
  downmix in the Gate 6 path. A full host decode with strict error explosion
  completed without a bitstream error, and its first-32-second hash matched the
  normal error-policy decode. Channel identities and layout stability are
  checked before pairing, and the supplied DME path has already passed native
  dependent-substream assembly as `5.1.2`.
- **Residual comparison limit**: A different conforming E-AC-3 implementation
  can still differ slightly in floating-point IMDCT, spectral extension and
  dither/noise realization. That remains a possible Cavern-versus-libav PCM
  difference, but not evidence for a replacement decoder. Isolate it by
  comparing synchronized multichannel base PCM before changing the core codec.
- **Cross-file limits**: Container skip/discard metadata can alter priming,
  endpoint length and OAMD-to-PCM alignment, although the current coded-domain
  pairing plus output-trim policy handles the supplied M4A and raw EB3 cases.
  FFmpeg also skips additional E-AC-3 independent programs with substream IDs
  other than zero; it does assemble a compatible dependent substream attached
  to the main ID-zero program. This does not drop channels from the supplied
  DME `5.1.2` sample, but future files must keep the substream inventory gate.

## Status refresh: 2026-08-25 (square horizontal reference-room A/B)

- **Corrected diagnostic assumption**: The Gate 7B reference geometry is now
  `10 x 10 x 7 m` with the listener at normalized `(0.5, 0.5, 0.0)`, giving
  listener-relative limits of `+/-5 m` horizontally/front-to-back and
  `+/-3.5 m` vertically. The previous `6 x 8 x 3 m` room was an arbitrary
  reproducibility fixture, not geometry recovered from OAMD, and distorted the
  normalized square horizontal plane into a rectangle.
- **Scope**: Only the explicit reference geometry and geometry-derived self-test
  expectations changed. JOC/OAMD decoding, object PCM, ramps, 15 dB headroom,
  static LFE handling, screen conversion, queueing and Spatial Audio ownership
  are unchanged. Both supplied files remain room-anchored
  (`screenConversions=0`).
- **Probe validation**: `Eac3SpatialPropertyProbe` passed all 42 cases. Release
  1,000-unit Gate 7B runs passed for Apple (`X=-5..5`, `Y=0`,
  `Z=-5..-3.225806`) and DME (`X=-5..5`, `Y=0..3.5`, `Z=-5..5`) with all
  expected updates committed and zero unsupported/rejected updates.
- **Real endpoint submission**: `build-luna-gate7c2\Release\
  Eac3AccessUnitProbe.exe 'media\POWDER SNOW Live V9.8.6.eb3'
  --max-units 1000 --joc-gate7c` passed 3,200/3,200 render commits,
  1,536,000 submitted frames, zero underrun, metric consistency and complete
  stop/reset/join/cleanup. This is **PASS** at the diagnostic Spatial Audio
  submission layer.
- **Comparative listening result**: The user reported that this square-room
  version is wider than the previous `6 x 8 x 3 m` version, confirming that the
  old horizontal aspect ratio affected the image. It is still less transparent
  than Cavern, so overall binaural acceptance remains **INCONCLUSIVE** and
  further coordinate enlargement is not justified by this result alone.
- **Binaural Render Mode exclusion**: The Logic master shows per-input
  Off/Near/Mid/Far Binaural Render Mode metadata, but Dolby documents that
  DD+JOC encoding does not use that master-level metadata. Cavern decoding the
  same `.eb3` therefore does not receive those Logic BRM choices either. Do not
  add an invented Near/Mid/Far mapping to the Windows object path; the remaining
  comparison is renderer voicing/HRIR/room processing unless a synchronized
  output analysis proves an upstream PCM difference.

## Status refresh: 2026-08-25 (headphone-metadata and Windows API boundary)

- **Previously untracked standard field**: ETSI TS 102 366 Annex H defines
  EMDF payload ID `0x7`, "headphone rendering data". It can carry per-channel
  pre-binaural gains, LFE gain, propagation delay, serialized early/late BRIR
  coefficients, and octave-band RT60 values. This is a public bitstream
  facility, not a Cavern-specific algorithm. ETSI TS 103 420 OAMD itself does
  not define an HRTF or binaural-room renderer; it ends at decoded object audio
  plus time-aligned object metadata.
- **Complete supplied-sample inventory**: The existing Release
  `Eac3AccessUnitProbe --emdf` scanned all 6,327 access units in
  `media\03. iPad.m4a` and all 14,000 access units in
  `media\POWDER SNOW Live V9.8.6.eb3`. Both reported exactly
  `emdfPayloadCounts=1,2,11,14`; neither contains payload `0x7`. Result:
  **PASS** for excluding omitted Annex-H headphone metadata as the cause of the
  current Cavern/Dolby Atmos for Headphones listening difference. Commands:
  `build-codex-gate7c2\Release\Eac3AccessUnitProbe.exe
  'media\03. iPad.m4a' --emdf` and the same command with
  `'media\POWDER SNOW Live V9.8.6.eb3'`.
- **Windows generic-object limit**: The selected-renderer
  `ISpatialAudioObject` path publicly exposes dynamic-object position and
  volume, plus fixed static-channel identity. It has no BRIR, RT60, room,
  reverb-trail, object-size, zone, snap, or distance-model submission method.
  Microsoft's documentation explicitly leaves environmental reverberation,
  distance filtering, occlusion and related cues to the content engine.
- **Media Foundation nuance**: `IMFSpatialAudioObjectBuffer` can carry
  `ISpatialAudioMetadataItems`, but the metadata is identified by a
  decoder-defined format GUID and must be supported by the active renderer.
  The public Windows documentation does not define a Dolby OAMD/JOC/BRIR
  metadata GUID that an independent decoder can synthesize. This interface is
  therefore not a public generic escape hatch for forwarding ETSI payloads to
  Dolby Atmos for Headphones.
- **Separate Microsoft-HRTF route**: `ISpatialAudioObjectForHrtf` does expose
  environment, distance decay, directivity and orientation, but Microsoft
  documents it as an explicit Windows Sonic for Headphones path. It does not
  support Dolby Atmos for Headphones, consumer renderer switching, or speaker
  output, so adopting it would replace rather than enrich the current Dolby
  renderer.
- **Conclusion / next comparison**: No required metadata from the two supplied
  files is currently being discarded at the Windows handoff. Keep the generic
  Dolby object route and test geometry/loudness with a synchronized Cavern
  stereo reference. Treat Cavern's HRIR, crossover, reflection and room model
  as an independent renderer implementation, not as missing ETSI requirements.

## Status refresh: 2026-08-25 (Cavern comparison reopens binaural acceptance)

- **Focused comparison**: After accepting the LFE-headroom correction itself,
  the user reported that Gate 7C2 still sounds muddier and spatially smaller
  than Cavern's Stereo headset rendering. Overall binaural quality therefore
  remains `INCONCLUSIVE`; the earlier subjective PASS applies only to the LFE
  balance improvement.
- **Renderer boundary**: Gate 7C2 submits decoded objects and listener-relative
  metre coordinates to Windows Spatial Audio; Dolby Atmos for Headphones owns
  the final HRTF and stereo render. It is not equivalent to Cavern's own
  headphone virtualizer. Current Cavern source uses its own HRIR convolution,
  preserves the sub-120-Hz band outside the spatial filters, and adds a quiet
  7.5 ms center reflection. These differences can affect both tonal balance and
  apparent externalization independently of decoder correctness.
- **Geometry hypothesis**: The diagnostic Windows mapping currently assumes a
  6 x 8 x 3 metre room and produces positions spanning approximately +/-3 m
  horizontally and +/-4 m front/back. Cavern's displayed environment spans
  10 m from the listener on each horizontal axis. This is a controlled A/B
  candidate, not yet a proven cause, because the two renderers interpret
  distance and environment size differently.
- **Reference requested**: The next comparison needs a Cavern-rendered stereo
  WAV from the same DME `.eb3`, with Headphone Virtualizer/Stereo headset,
  Cavernize/Disassembler/Nearest disabled, no room correction or output EQ, and
  the exact Cavern version and Trails/Silence fade settings recorded. Compare
  against a fresh synchronized Gate 7C2 loopback after loudness alignment.
- **Hot-switch limitation**: Changing the Windows spatial sound format can
  invalidate the active Spatial Audio stream. The diagnostic currently records
  the failed update and exits; it does not rebuild. Production integration must
  treat `SPTLAUDCLNT_E_DESTROYED`, `AUDCLNT_E_DEVICE_INVALIDATED`, and
  `AUDCLNT_E_RESOURCES_INVALIDATED` as a high-level output recovery transaction:
  stop/release the old stream and objects, reopen capabilities and format,
  prebuffer the current generation, then resume. If the reported switch was
  only a Dolby Access EQ/content profile, it must be tested separately because
  that profile is not an application-visible Spatial Audio stream property.

## Status refresh: 2026-08-25 (Gate 7C2 LFE headroom mismatch corrected)

- **Focused listening finding**: After the initial basic acceptance, the user
  reported that LFE was too strong, the result sounded muddy, and the stage felt
  close. Code inspection found that all zero-gain dynamic objects used the
  required 15 dB program headroom (`SetVolume(0.177827941)`), while the static
  LFE never called `SetVolume()` and therefore remained at the Windows default
  `1.0`. This made LFE 15 dB louder relative to zero-gain objects before Dolby
  Atmos for Headphones rendering.
- **Localized correction**: Gate 7C2 now applies `0.177827941` to the static LFE
  once, immediately after activation and inside the first open update. Metrics
  expose the actual value and require exactly one successful LFE volume call.
  Object PCM, LFE PCM, OAMD gains, coordinates, room geometry, queueing, and
  endpoint selection are unchanged.
- **Endpoint validation**: A Release run of
  `Eac3AccessUnitProbe 'media\POWDER SNOW Live V9.8.6.eb3' --max-units 1000
  --joc-gate7c` passed Gate 6C, Gate 7B, and Gate 7C with 1,536,000 submitted
  frames, zero underrun, `gate7cLfeVolumeCalls=1`,
  `gate7cLfeVolume=0.177828`, metric consistency, and complete stream stop/reset/
  join/cleanup.
- **Project validation**: `scripts\validate-all.ps1 -BuildDir
  build-codex-lfe-headroom-tests -SkipSmoke` passed all 10 unit suites and the
  report-schema self-test; aggregate report:
  `build-codex-lfe-headroom-tests/validation-report.json`.
- **Acceptance boundary**: The correction is objectively verified at the
  Spatial Audio submission layer. In the immediate same-file follow-up, the
  user reported `好多了。应该是对的。`; record this as a subjective PASS for
  the LFE-headroom correction. It is not yet a controlled localization test or
  certification of future static-bed handling.

## Status refresh: 2026-08-25 (Gate 7D real-object loopback and basic listening accepted)

- **Synchronized evidence**: `WasapiLoopbackCapture` was ready before the final
  1,000-unit DME Gate 7C2 run and captured the same endpoint ID
  `{0.0.0.00000000}.{22726180-998c-4f83-8bd4-1ec7620b8041}` without interruption.
  Gate 7C2 itself passed 1,536,000 submitted frames, zero underrun, metric
  consistency, and complete stop/reset/join/cleanup.
- **Endpoint output**: The capture contains 1,824,000 stereo float32 frames at
  48 kHz / 38 seconds, peak `0.628382`, maximum block RMS `0.199903`, no silent
  packets, and zero dropout candidates. This proves that the real decoded-object
  run produced non-silent stereo endpoint output through the selected spatial
  route, not merely successful API submission.
- **Detector limitation**: The generic report result is `FAIL` because it found
  seven `sudden-sample-delta` packet candidates in two musical regions around
  5.87-6.14 and 22.82-22.85 seconds, plus the expected post-playback tail. A
  direct sample inspection found repeated alternating large deltas inside the
  capture packets rather than isolated discontinuities on 10 ms renderer
  boundaries. That is more consistent with program transients, but it does not
  override the detector or independently prove pop-free playback.
- **Human result**: Immediately after the synchronized real-object run, the
  user reported `我感觉挺好的`. Record this as PASS for basic subjective
  acceptance of the rendered result and absence of an obvious listening
  objection. It is informal feedback, not a controlled stereo/object A/B, a
  separate certification of every detector timestamp, or proof of binaural
  localization accuracy. The seven generic detector candidates remain visible
  as a diagnostic limitation rather than being rewritten as audible defects.
- **Artifacts**: Report and captured WAV are under
  `build-codex-gate7c2/gate7d-loopback-20260825-133947-269/`; focused 1.1-second
  clips start at 5.5 and 22.4 seconds. These are local generated evidence and
  remain outside Git.
- **Gate 7D conclusion / next implementation boundary**: Non-silent endpoint
  output plus basic human listening are accepted. Production playback will
  reuse source preparation and the FFmpeg/libav control-plane shape, while
  replacing its interleaved PCM buffer/output sink with the proven Gate 6C batch queue and
  Spatial Audio consumer. The staged plan is in
  `docs/dev/eac3-joc-production-playback.md`.

## Status refresh: 2026-08-25 (Gate 7C2 real Spatial Audio bridge passed)

- **Implementation**: `Eac3AccessUnitProbe --joc-gate7c` now connects the
  existing Gate 6C 15-object-plus-LFE PCM and Gate 7B properties to an
  event-driven `ISpatialAudioObjectRenderStream`. One consumer thread owns COM,
  endpoint/stream/event/notify objects and all 16 spatial objects; the decoder
  producer communicates only through the bounded Gate 7C1 queue. Defaults are
  eight batches, four prebuffered batches, and a 2,000 ms push timeout.
- **Reviewed lifecycle**: Each successful Begin has one End, every live object
  receives the exact requested mono-float buffer length, and Stop/Reset/release/
  join complete on the consumer side. Review also fixed an endpoint-ID UTF-8
  terminator overrun and a nested-lock deadlock in the initialization-timeout
  path. The final renderer-contract self-test passed 12 cases; the bridge core
  passed 53 cases including the initial-property readiness mask.
- **Final DME endpoint run**: Independent Release execution of the first 1,000
  units used endpoint
  `{0.0.0.00000000}.{22726180-998c-4f83-8bd4-1ec7620b8041}`, prebuffered four
  batches / 5,567 frames (115.979 ms), and submitted 3,200 exact 480-frame
  updates. Source/submitted frames were both 1,536,000; 51,200 object-buffer
  calls covered exactly 98,304,000 bytes and 96,000 property calls. All 16
  finite counts were 1,536,000. Producer real-time ratio was 0.992010 under
  bounded backpressure. Push timeout, validation/flush rejection, underrun,
  pending EOS metadata, remaining/discarded items, and inactive objects were
  all zero; maximum causal lateness was 384 frames and metric consistency,
  the single terminal quantum, Stop/Reset/join/cleanup all passed.
- **Apple endpoint run**: The independent 1,000-unit Release run honored the
  2,192-frame leading trim, submitted 1,533,808 source frames plus only 272
  final padding frames, and committed 3,196 updates. Its 51,136 exact buffer
  calls covered 98,181,120 bytes and 95,880 property calls; maximum lateness
  was 464 of 480 frames. Push timeout, rejection, underrun, EOS residue and
  discarded work were zero, with one terminal quantum and complete cleanup.
- **Project validation**: `scripts\validate-all.ps1 -BuildDir
  build-codex-gate7c2-tests -SkipSmoke` passed all 10 test suites and the
  report-schema self-test; smoke was skipped. Aggregate report:
  `build-codex-gate7c2-tests/validation-report.json`.
- **Evidence limit / next phase**: These results prove real decoded object PCM
  and metadata were accepted by the selected Windows Spatial Audio endpoint;
  they do not by themselves prove audible output or correct Dolby Atmos for
  Headphones binaural placement. Next, synchronize Gate 7C2 with WASAPI
  loopback and listening evidence, then move the proven path behind production
  playback lifecycle rather than widening the diagnostic further.

## Status refresh: 2026-08-25 (Gate 7C1 bounded bridge core passed)

- **Gate 7B correction**: The property adapter now accepts valid sparse and
  multi-position object updates instead of requiring all 15 objects at every
  source position. Flush-tail PCM may carry previously decoded metadata and is
  processed through the same adapter path. Duplicate/descending indices,
  malformed properties, and late unsupported updates still reject the whole
  batch transactionally. Gate 7B self-tests increased from 37 to 42 cases.
- **Bridge implementation**: Added the Windows-independent
  `Eac3SpatialBridgeCoreProbe`, a fixed-capacity SPSC batch queue, generation/
  close/cancel lifecycle, transactional input admission, and a scheduler that
  splits/coalesces Gate 6C PCM into exact render quanta while sampling Gate 7B
  properties causally at quantum starts.
- **Bridge evidence**: The 52-case self-test passed bounded FIFO and real
  waiter wakeups, stale generation/reset, gap/overlap and terminal-flush
  admission, 15 distinct object planes plus LFE, sparse/flush metadata,
  exact ramp catch-up, final padding, underrun without source-time advance,
  and explicit cancel current/pending plus EOS-pending accounting. Source
  frames staged during a concurrent cancellation are separately counted by
  the bridge implementation.
- **Real regression**: Independent Release 1,000-unit runs retained Gate 6C
  and Gate 7B PASS for both supplied streams. Apple committed 14,985/14,985
  updates in 4.62 seconds; DME committed 15,000/15,000 in 5.57 seconds. Both
  had zero overlap, unsupported, or rejected updates and stable identities
  1 through 15.
- **Project validation**: `scripts\validate-all.ps1 -BuildDir
  build-codex-gate7c1-tests -SkipSmoke` passed all 10 test suites and the
  report-schema self-test; aggregate report:
  `build-codex-gate7c1-tests/validation-report.json`.
- **Evidence limit / next phase**: Gate 7C1 is an offline bridge-core proof. It
  does not open Spatial Audio or produce sound. Gate 7C2 will keep COM and all
  endpoint objects on one consumer thread, prebuffer four of eight batches,
  and require zero underrun plus exact End-update and teardown accounting.

## Status refresh: 2026-08-25 (Gate 7B property adapter passed)

- **Implementation**: Added the renderer-neutral `Eac3SpatialPropertyProbe`
  and a transactional 15-object adapter behind
  `Eac3AccessUnitProbe --joc-gate7b`. It maps explicit ETSI room/screen
  geometry to listener-relative Windows metres, applies 15 dB gain headroom,
  preserves sample-position ramp state, and rejects unsupported properties or
  malformed whole batches without calling Windows Spatial Audio.
- **Self-test**: The independent adapter probe passed 37 cases covering room
  basis/handedness, outside-room coordinates without clamping, screen/depth
  interpolation, gain endpoints and finite/silent ramps, overlapping ramps,
  all 15 identities, arithmetic/property rejection, transaction rollback, and
  reset equivalence.
- **Real Apple evidence**: The first 1,000 units committed 14,985/14,985
  updates, 999 per object, with 15 first-state snaps, 14,970 ramps, zero
  overlap/unsupported/rejected updates, and finite Windows ranges X
  `-3..3`, Y `0.3..0.3`, Z `-4..-2.580645`, volume `0.177828`.
- **Real DME evidence**: The first 1,000 units committed 15,000/15,000
  updates, 1,000 per object, with 15 first-state snaps, 14,985 ramps, zero
  overlap/unsupported/rejected updates, and finite Windows ranges X
  `-3..3`, Y `0.3..1.8`, Z `-4..4`, volume `0.177828`.
- **Corrected screen interpretation**: Both supplied streams have the
  screen-reference field present but `useScreenReference=false` (diagnostic
  mode 1), so all real updates correctly used room conversion. The previous
  statement that these samples were screen-anchored was wrong. Screen mode 2
  conversion is tested synthetically against explicit reference geometry.
- **Independent commands**: Codex built `Eac3SpatialPropertyProbe` and
  `Eac3AccessUnitProbe` under `build-codex-gate7b`, ran the 37-case probe, and
  ran both supplied samples with `--max-units 1000 --joc-gate7b`; all returned
  `PASS` with `evidenceLimit=offline-property-adapter-only`. Full project
  validation passed all 10 test suites and the report-schema self-test with
  smoke skipped; aggregate report:
  `build-codex-gate7b-tests/validation-report.json`.
- **Evidence limit / next phase**: Gate 7B does not submit decoded PCM to the
  spatial endpoint and does not prove sound, binaural placement, real-time
  throughput, or Dolby Atmos for Headphones. Gate 7C is next: join Gate 6C PCM
  and Gate 7B properties through a bounded, reset-safe queue to the already
  proven Gate 7A diagnostic dynamic-object sink.

## Status refresh: 2026-08-25 (Gate 7A Windows dynamic sink passed)

- **Implementation**: Added the Windows-only diagnostic
  `SpatialDynamicProbe`. It opens the default spatial endpoint directly with
  `ISpatialAudioObjectRenderStream`, requests 15 dynamic objects plus one
  static LFE object, and submits generated mono float32/48-kHz PCM, bounded
  listener-relative positions, and `[0, 1]` object volumes. It is independent
  of Media Foundation, FFmpeg, Gate 6C output, and production playback.
- **Endpoint submission evidence**: The current default endpoint reported a
  native static mask of `0xffffe`, static LFE support, and 128 maximum dynamic
  objects. An independent 8-second Codex run activated all 15 dynamic objects
  plus LFE and passed 800/800 update transactions, 384,000 submitted frames,
  12,800 object-buffer calls, 12,000 position calls, 12,000 volume calls, zero
  wait timeouts, and zero inactive objects. All 6,144,000 generated values were
  finite, the per-object source peak was 0.018, count consistency passed, and
  stream reset/cleanup completed.
- **Endpoint-output evidence**: A synchronized `WasapiLoopbackCapture` run on
  the same endpoint captured 404,160 stereo frames / 8.42 seconds of non-silent
  output with peak 0.338873 and no capture interruption. Its detector result is
  deliberately `INCONCLUSIVE` because it flagged the stopping tail as a
  fade candidate; zero transient/dropout candidates and non-silent loopback do
  not prove audible quality or correct binaural placement. Report:
  `build-codex-gate7a/gate7a-loopback-codex2/report.json`.
- **Self-test and regression**: `SpatialDynamicProbe --self-test` passed 15
  endpoint-free checks, including argument limits, deterministic finite PCM,
  15 stable identities, 3-D position bounds, whole-pass transaction metrics,
  exact buffer length/count formulae, capacity/non-finite rejection, and reset
  equivalence. `scripts\validate-all.ps1 -BuildDir
  build-codex-gate7a-tests -SkipSmoke` passed all 10 project test suites and the
  report-schema self-test; aggregate report:
  `build-codex-gate7a-tests/validation-report.json`.
- **Evidence limit / next phase**: Gate 7A proves that the selected Windows
  spatial renderer accepts and emits a bounded 15-object-plus-LFE synthetic
  stream. It does not prove Dolby decoding, OAMD coordinate/gain conversion,
  Dolby Atmos for Headphones binaural quality, or the Gate 6C real-time bridge.
  Gate 7B is next: implement and test the explicit OAMD-to-Windows property and
  geometry adapter before connecting real decoded objects.

## Status refresh: 2026-08-25 (Gate 6C aligned object stream passed)

- **Implementation**: Added a diagnostic renderer-neutral assembler that joins
  the 15 Gate 5C object-QMF essences, the index-0 LFE helper/bypass, and Gate
  6B2B object-property snapshots. It applies the measured 577-sample QMF
  synthesis delay to LFE and metadata, preserves one ordered update per object
  and update block, performs one common leading/terminal trim, flushes the QMF
  tail with zero input, and exposes whole transactional callback batches.
- **Timing semantics**: Metadata source position is access-unit start plus the
  decoded sample offset plus `32 * blockOffsetFactor`. Sample-offset and ramp
  duration table forms are converted to samples; reserved or inconsistent
  encodings are rejected. The latest pre-trim state for each object is carried
  to output sample zero, while the zero-QMF tail emits no new metadata.
- **Synthetic evidence**: `Eac3JocGate6cProbe` passed exactly 20 cases covering
  15-object/LFE shape, true QMF-analyzed impulse alignment, 577-sample flush,
  eight blocks and 120 ordered metadata updates, sparse pre-trim state,
  a real late update carried into the flush interval without generating
  synthetic metadata, terminal discard, timing-table conversion, callback
  rejection, malformed transaction rollback, unit gaps, and reset/fresh
  equivalence.
- **Real evidence**: `Eac3AccessUnitProbe --max-units 1000 --joc-gate6c`
  passed both supplied samples with zero fallback and zero metadata-order
  failures. Apple config 3 emitted 1,533,808 samples per object/LFE and 14,985
  aligned updates after its 2,192-sample leading trim. DME config 4 emitted
  1,536,000 samples per object/LFE and all 15,000 updates. All object and LFE
  samples were finite, both output ranges were exactly contiguous from sample
  zero to the expected end, and both runs completed the 577-sample tail flush.
  Apple object PCM ranged from -0.403119 to 0.301648 (peak 0.403119); its
  supplied LFE plane was zero. DME object PCM ranged from -0.0789883 to
  0.10486 (peak 0.10486), and LFE ranged from -0.0120878 to 0.012883.
- **Regression and project validation**: Independent Codex runs passed B2B
  (37), B2A (19), B1 (15), Gate 6A (12), and Gate 4 at
  73.10/83.61/78.24/78.30 dB with delay 577. The Gate 6C project validation
  run passed all 10 suites and the report schema with smoke skipped; aggregate
  report:
  `build-codex-gate6c-tests/validation-report.json`.
- **Evidence limit / next phase**: This proves the offline decoder-side object,
  LFE, metadata, delay, trim, and transaction contract. It does not yet feed
  Windows dynamic spatial objects, invoke Dolby Atmos for Headphones, or prove
  audible output. The next phase is a separate Windows renderer adapter and
  real-time throughput/backpressure gate; CUDA or vendor SIMD is not required
  for the completed correctness proof.

## Status refresh: 2026-08-25 (Gate 6B2B object-property state passed)

- **Implementation**: Added a transactional state layer over B2A raw records.
  It resolves basic/render default, full, reuse, and mixed updates; absolute
  and differential position; gain, priority, distance, zone, scalar/3-D size,
  elevation, screen reference, depth, and snap. Raw codewords remain attached
  to each normalized state, and every one-to-eight update block is retained as
  an ordered snapshot rather than collapsing the frame to its final state.
- **Corrected semantics**: Codex review corrected X/Y and Z coordinate divisors
  to 62 and 15, size to 31, coded priority to 32, all six Table-20 zone maps,
  screen/distance recomputation, and the requirement that differential updates
  have a prior valid render state. Block zero is enforced as the B2A full/
  absolute baseline; reuse, mixed, and differential occur only later.
- **Mapping and transactionality**: OAMD index 0 remains the LFE-only bed
  helper and is excluded from the stable dynamic index list 1 through 15.
  Object/helper shape change and explicit reset are fresh-instance equivalent.
  Late malformed updates leave both persistent state and caller output
  unchanged.
- **Synthetic evidence**: `Eac3OamdB2bProbe` passed exactly 37 cases covering
  default/full/reuse/mixed, gain and priority codewords, exact formula bounds,
  differential history, all zone maps, distance/infinity, screen toggles,
  scalar/3-D size, snap, inactive/reactivation, one/eight block snapshots,
  shape/reset, 159-object bounds, and transactional rejection.
- **Real evidence**: For each of the first 1,000 access units of both supplied
  samples, B2B applied 1,000/1,000 frames with zero fallback and one initial
  reset. Each produced 16,000 finite active states: 1,000 default-render LFE
  helpers and 15,000 full/absolute dynamic-object updates in stable order.
- **Regression**: Independent Codex runs passed B2A (19), B1 (15), Gate 6A
  (12), and Gate 4 at 73.10/83.61/78.24/78.30 dB with delay 577. Full project
  validation passed 10 suites and report schema at
  `build-codex-gate6b2b-tests/validation-report.json`; smoke was skipped.
- **Evidence limit / next phase**: The real streams contain one full update
  block, so reuse/mixed/differential/multi-block evidence is synthetic. No
  object PCM is associated with metadata yet, and no renderer is called. Gate
  6C will join object PCM, delayed LFE, OAMD timing, and container trim on one
  diagnostic timeline.

## Status refresh: 2026-08-25 (Gate 6B2A raw OAMD updates passed)

- **Implementation**: Added a bounded parser for the owned ID-1 element body,
  covering sample/update-block timing, ramp coding, active state, raw
  basic/render statuses, every conditional gain/priority/position/distance/
  zone/size/screen/snap codeword, reserved data, additional data, and zero
  element padding. Results remain raw and keyed by object and block; no
  persistent property state is applied yet.
- **Dynamic-only plus LFE ordering**: The ID-1 element still carries all 16
  declared object records. Object index 0 is the LFE-only bed helper and
  indices 1 through 15 are the dynamic objects. An exhaustive scan of every
  possible single-helper index found index 0 to be the only map that consumed
  the element boundary for both supplied samples under both candidate bit
  orders; parsing only 15 records was a false alignment.
- **Bit-order limit**: Clause 5.5.11 and Table 31 label the four render-field
  presence bits in opposite directions. The parser makes the policy explicit,
  defaults to the clause-5.5.11 LSB interpretation, and retains the Table-31
  interpretation as a diagnostic option. The supplied one-block/full-update
  streams cannot distinguish them.
- **Synthetic evidence**: `Eac3OamdB2aProbe` passed 19 cases covering all
  field/status branches, both presence-bit policies, one/eight update blocks,
  truncation and arithmetic bounds, reserved/additional data, padding,
  LFE-helper mapping, and wrong-helper rejection.
- **Real evidence**: For each of the first 1,000 access units of both the Apple
  M4A and DME EB3, all 1,000 ID-1 elements passed with zero malformed or
  unsupported frames. Each sample produced 16,000 records, 487,000 consumed
  bits, 3,000 zero padding bits, 1,000 index-0 helper mappings, and 1,000/1,000
  unit associations. Render status 0 occurred 1,000 times for index 0 and
  status 1 occurred 15,000 times for indices 1 through 15.
- **Regression**: Gate 6B1, Gate 6A, and Gate 4 probes passed. Full validation
  passed all 10 unit suites and report-schema self-test at
  `build-codex-gate6b2a-tests/validation-report.json`; playback smoke was
  intentionally skipped for this offline diagnostic.
- **Evidence limit / next phase**: B2A proves bounded raw syntax only. It does
  not apply default/reuse/mixed status, differential history, normalized
  properties, trim/extended semantics, PCM alignment, or a renderer. Gate
  6B2B adds transactional object-property state; Gate 6C remains responsible
  for PCM/timing association.

## Status refresh: 2026-08-25 (Gate 6B1 OAMD framing passed)

- **Implementation**: Added a bounded OAMD payload-11 framing parser and an
  `Eac3AccessUnitProbe --oamd` inventory path. It parses version and escaped
  counts, all program-assignment branches, byte-bounded OA element metadata,
  optional alternate IDs, discard policy, and zero final padding while keeping
  object/trim/extended bodies opaque for the next phase.
- **Syntax boundaries**: The parser distinguishes framing maxima from renderer
  limits: 159 OAMD objects, 159 dynamic objects, 46 OA elements, and the full
  bounded `variable_bits_max(4,4)` size range. Unsupported versions, reserved
  ISF indices, non-default alternate IDs, and unknown elements are reported as
  `UNSUPPORTED`; truncation, size overrun, and non-zero padding are
  `MALFORMED`.
- **Synthetic evidence**: `Eac3OamdB1Probe` passed 15 cases covering normal and
  extended counts, maximum counts/size code, all program-assignment branches,
  recognized and discardable/non-discardable unknown elements, unsupported
  version/reserved values, truncation, declared-size overrun, and padding.
- **Real inventory**: For each of the first 1,000 access units of both the
  Apple M4A and DME EB3, payload 11 was version 0, dynamic-only with LFE, and
  declared 16 objects. Every frame contained exactly one object element
  `(ID 1, 61 bytes)` and one trim element `(ID 2, 1 byte)`. All 2,000 elements
  per sample were recognized; there were zero malformed/unsupported frames,
  zero duplicate-payload units, and 1,000/1,000 unit associations.
- **Regression**: Gate 6A retained 12 synthesis cases and delay 577; Gate 4
  retained its SNR and split/reset invariants. Full project validation passed
  10 unit suites and report-schema self-test with playback smoke intentionally
  skipped.
- **Evidence limit / next phase**: B1 proves payload and element framing only.
  It does not interpret object timing, active state, gain, position, trim, or
  update/reuse. Gate 6B2A will parse raw object-element update syntax and
  inventory it before stateful value reconstruction.

## Status refresh: 2026-08-25 (Gate 6A object-QMF synthesis passed)

- **Implementation**: Added a diagnostic-only object synthesis module with
  one persistent shared-QMF synthesis bank per Gate 5C object. Each successful
  input produces exactly 1,536 planar float samples per object. Zero-QMF
  objects still advance state; reset and object-count changes rebuild all
  banks before the current frame.
- **Transactional state**: Complete QWIN, object-count, dimension, and complex
  finite validation happens before synthesis. Work runs on cloned banks and
  commits state and caller output only after every object succeeds, so a late
  malformed value or non-finite output cannot contaminate the following frame.
- **Synthetic evidence**: `Eac3JocSynthesisProbe` passed 12 cases covering
  zero QMF, the Gate 4 reference chain, three independent objects, continuous
  versus split calls, explicit reset, object-count reset, 16-object maximum,
  exact 1,536-sample output, transactional late-NaN rejection, and rejection
  of 0/17 objects, a 1,535-value object, and a non-finite QWIN table.
- **Shared latency contract**: Gate 4 and Gate 6A now reference the same
  `577`-sample analysis/synthesis delay constant. Gate 6A records this delay
  but does not yet shift OAMD, LFE, or container trim.
- **Regression**: The unchanged Gate 4 probe remained at
  73.10/83.61/78.24/78.30 dB with delay 577 and exact split/reset invariants.
  `scripts\validate-all.ps1 -BuildDir build-luna-gate6a-tests -SkipSmoke`
  passed all 10 unit suites and the report-schema self-test; playback smoke was
  intentionally skipped because Gate 6A is an offline diagnostic.
- **Evidence limit / next phase**: No OAMD syntax, real object/OAMD timing,
  production playback, or Windows renderer is implemented. Gate 6B1 will first
  inventory bounded OAMD top-level and element framing before object-property
  state is attempted.

## Status refresh: 2026-08-25 (Gate 6 execution split)

- **Decision**: Gate 6 is split into 6A object-QMF synthesis, 6B bounded OAMD
  parsing/state, and 6C renderer-neutral timing/alignment. Luna receives one
  phase at a time, followed by Codex diff review and local validation.
- **Active scope**: Gate 6A only reuses the exact Gate 4 synthesis convention
  to produce 1,536 finite PCM samples per Gate 5C object with persistent,
  transactional per-object state. It records the measured 577-sample latency
  but does not yet shift metadata or LFE.
- **Deferred scope**: OAMD supports shared timing and up to eight property
  updates per object in one codec frame, so syntax/state parsing is isolated in
  Gate 6B. Payload pairing, container trim, LFE/metadata delay alignment, and
  30-second renderer-neutral output are Gate 6C.
- **Evidence limit**: This entry is an implementation contract, not a Gate 6A
  build or object-PCM result. No production or Windows renderer path changes.

## Status refresh: 2026-08-24 (Gate 5C object-QMF reconstruction passed)

- **Implementation**: Added reusable Gate 4 QMF analysis state plus bounded
  Pseudocode-7 object-QMF reconstruction. `Eac3AccessUnitProbe --joc-qmf`
  now associates each JOC payload and Gate 5B matrix with its access-unit
  index, decodes unmodified planar float PCM, maps channels from explicit
  `AVChannel` identities, excludes LFE from the matrix, and retains LFE as a
  separately checked bypass signal. The diagnostic remains outside production
  playback, OAMD, object PCM synthesis, and Windows rendering.
- **Codec-priming decision**: Object reconstruction uses untrimmed coded-domain
  PCM so every matrix sees its original 1,536 samples and the persistent QMF
  history begins at access unit zero. Container `skip_samples` and discard
  padding are recorded for application after future object synthesis. On the
  Apple sample, applying the 2,192-sample trim before QMF produced an 880-sample
  first frame and only 998 complete pairings; deferring it restored strict
  1,536-sample pairing for all 1,000 units.
- **Synthetic and Gate 4 regression**: `Eac3JocQmfProbe` passed 16 cases,
  including exact config-0-through-4 identity order, reset equivalence,
  transactional malformed-matrix rejection, reserved configs 5 through 7,
  zero/copy/identity/cancellation matrices, and non-finite rejection. The
  unchanged Gate 4 probe remained at 73.10/83.61/78.24/78.30 dB with a
  577-sample delay and zero split/reset difference.
- **Apple Music config 3**: Native `5.1(side)` PCM mapped to
  `FL,FR,FC,SL,SR` plus LFE bypass. All 1,000 access units were found, paired,
  and reconstructed continuously with 15 objects, 23,040,000 finite complex
  object-QMF values, 1,536,000 finite LFE samples, one initial QMF reset, no
  sequence discontinuity, and zero fallback.
- **DME EB3 config 4**: Native `5.1.2` PCM mapped to
  `FL,FR,FC,SL,SR,TFL,TFR` plus LFE bypass. It produced the same
  1,000/1,000/1,000 unit result, 15 stable objects, 23,040,000 finite complex
  object-QMF values, 1,536,000 finite LFE samples, one initial reset, and zero
  fallback or discontinuity.
- **Local validation**:

  ```powershell
  .\build-luna-gate5c\Debug\Eac3JocQmfProbe.exe `
    --table 'docs\dev\ts_103420_tables.c'
  .\build-luna-gate5c\Debug\Eac3QmfProbe.exe `
    --table 'docs\dev\ts_103420_tables.c'
  .\build-luna-gate5c\Debug\Eac3AccessUnitProbe.exe `
    'media\03. iPad.m4a' --max-units 1000 --joc-qmf
  .\build-luna-gate5c\Debug\Eac3AccessUnitProbe.exe `
    'media\POWDER SNOW Live V9.8.6.eb3' --max-units 1000 --joc-qmf
  scripts\run-tests.ps1 -BuildDir build-luna-gate5c-tests
  ```

  Both real probes returned
  `jocQmfResult=PASS stage=gate5c-object-qmf-reconstruction`; all 10 project
  test suites passed.
- **Evidence limit / next gate**: Gate 5C proves deterministic complex
  object-QMF reconstruction only. It does not yet produce object PCM, parse
  OAMD positions, submit Windows spatial objects, exercise Dolby Atmos for
  Headphones, or prove audible output. Gate 6 is the next decoder slice; no
  CUDA, SIMD, FFmpeg patch, or production renderer change was required here.

## Status refresh: 2026-08-24 (Gate 5C scope unblocked for configs 3/4)

- **Decision**: Gate 5C no longer waits for config-0/config-1 media. The two
  supplied real streams are now the required validation paths: Apple Music
  config 3 over native `5.1(side)` PCM and the DME Blu-ray config 4 stream over
  native `5.1.2` PCM. Gate 3 already verified both layouts and dependent
  substream handling for the EB3 path.
- **Configuration policy**: Gate 5C maps configs 0 through 4 by explicit
  channel identity. Configs 3 and 4 require 1,000-access-unit real validation;
  configs 0, 1, and 2 may have synthetic coverage without delaying the current
  decoder. Configs 5 through 7 remain reserved and rejected.
- **Phase handling**: The 90-degree indication in configs 3 and 4 describes
  Ls/Rs preprocessing already present before E-AC-3 encoding. The JOC decoder
  receives decoded channel QMF directly and TS 103 420 does not specify an
  inverse phase operation before Pseudocode 7. Gate 5C must not add one.
- **Next implementation slice**: Reuse the Gate 4 complex analysis bank, pair
  native channels by identity, bypass LFE, and reconstruct bounded complex QMF
  for all 15 JOC objects. OAMD, object PCM synthesis, Media Foundation,
  Windows Spatial Audio, and production playback remain outside this slice.
- **Evidence limit**: This entry records the reviewed implementation scope and
  corrects the previous sample-blocking decision. It is not a Gate 5C build,
  object-QMF, renderer, or listening result.

## Status refresh: 2026-08-24 (Gate 5B coefficient mathematics)

- **Probe**: Added `joc-gate5b.cpp/.h` and retained the decoded JOC data-point
  values from Gate 5A. The diagnostic now implements sparse/non-sparse
  differential reconstruction, the two ETSI dequantizers, all eight Table 54
  parameter-band mappings, smooth and steep one/two-point interpolation, and
  persistent sequence-aware matrix state. It remains outside QMF, playback,
  OAMD, and Media Foundation routing.
- **Synthetic math self-test**: Mapping cases `512` and interpolation/state
  cases `6` passed, including quantizer center/lower endpoint, sparse modulo
  differential coding, both interpolation modes and data-point counts, and
  the 1023-to-1 sequence wrap plus sequence-zero reset:
  `jocMathSelfTest=PASS`.
- **Apple Music M4A**: Over the first 100 access units, config 3 was parsed as
  five channels with zero malformed frames. All 100 frames passed Gate 5B
  math (`jocMathPassCount=100`, `jocMathFailCount=0`).
- **Raw EB3**: Over the first 100 access units, config 4 was parsed as seven
  channels with zero malformed frames. All 100 frames also passed the pure
  math stage; the first frame reset the matrix state as signalled by sequence
  zero.
- **Evidence limit / stop condition**: Both supplied real samples use
  recognized-but-unsupported Gate 5C downmix configurations (3 and 4), not
  configurations 0 or 1. The handoff explicitly requires stopping rather
  than guessing the additional channel/phase rules. No object-QMF matrix was
  connected, and no renderer or production path was changed.
- **Next decision**: Obtain a real config-0 or config-1 JOC sample before
  implementing Gate 5C and claiming real object-QMF continuity.

## Status refresh: 2026-08-24 (Gate 5A bounded JOC syntax/Huffman)

- **Probe**: Added `joc-gate5a.cpp/.h` to the diagnostic
  `Eac3AccessUnitProbe`. It consumes the already extracted EMDF payload 14
  bytes, parses the bounded JOC header/info/data syntax, selects the six
  Annex A.1 Huffman tables from the local reference, records exact consumed
  bits and padding pattern, and rejects malformed walks without changing
  QMF, playback, or FFmpeg code.
- **Self-test**: All six tables loaded with 588 reachable leaves and passed
  MSB-first codeword round trips. Fixed cycle, out-of-range-node, and
  truncated-codeword, excessive-object, reserved-config, excessive-padding,
  and valid config-0 cases were checked:
  `jocHuffmanSelfTest=PASS`.
- **Apple Music M4A**: The first 100 access units produced 100 payload-14
  reports, all with config 3 (5 channels, 15 objects), syntax lengths from
  1,070 to 1,472 bits, padding from 0 to 7 bits, and zero malformed payloads.
  Config 3 is recognized but
  intentionally unsupported in Gate 5A; sequence values were continuous
  across the sampled range. The report ended with
  `jocResult=PASS stage=gate5a-joc-syntax` because the bounded syntax checks
  passed; this is not a claim that config 3 was rendered.
- **Raw EB3**: The first 100 access units produced 100 payload-14 reports,
  all with config 4 (7 channels, 15 objects), zero malformed payloads, and
  one initial sequence reset. The report also includes per-object presence,
  syntax-bit range, legal 1023-to-1 wrap count, splice/reset count, and
  discontinuity count. Config 4 is likewise recognized but outside
  this gate's supported render configurations. The same Gate 5A result line
  was returned.
- **Commands/evidence**:

  ```powershell
  cmake -S . -B build-luna-gate5a `
    -DCMAKE_PREFIX_PATH=D:/Qt/6.11.0/msvc2022_64 `
    -DAUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT=build-mm/ffmpeg-audio-core/runtime-with-ffprobe-msvc `
    -DAUDIOPLAYER_REQUIRE_LIBAV_DECODER=ON
  cmake --build build-luna-gate5a --target Eac3AccessUnitProbe --config Debug -- /m:1
  .\build-luna-gate5a\Debug\Eac3AccessUnitProbe.exe `
    'media\03. iPad.m4a' --max-units 100 --joc --joc-self-test
  .\build-luna-gate5a\Debug\Eac3AccessUnitProbe.exe `
    'media\POWDER SNOW Live V9.8.6.eb3' --max-units 100 --joc --joc-self-test
  ```

- **Evidence limit**: Gate 5A proves payload framing, bounded syntax, and
  table traversal only. Neither sample reached a supported config 0/1, and
  no coefficient mathematics, QMF object matrix, object PCM, or Dolby
  Atmos for Headphones handoff has been implemented.
- **Next gate**: Review the parser output, then implement Gate 5B pure
  coefficient/dequantization/interpolation mathematics in a separate change.

## Status refresh: 2026-08-24 (Gate 3 native PCM pairing)

- **Probe**: `Eac3AccessUnitProbe --pcm` now demuxes the same compressed
  packets, feeds them unchanged to libavcodec, and compares native planar
  float output against assembled 1536-sample access units. Skip-sample side
  data is preserved; no `SwrContext`, resampler, or channel downmix is used.
- **Apple Music M4A**: Over 1000 units / 32.000 seconds, native output was
  48 kHz, six-channel `5.1(side)`, `fltp`. It produced 1,533,808 samples
  after 2,192 skip samples, with the first frame at 880 samples:
  `pcmResult=PASS`, `pcmAccessUnitPairing=PASS_WITH_CODEC_PRIMING`.
- **Raw EB3**: Over 1000 units / 32.000 seconds, native output was 48 kHz,
  eight-channel `5.1.2`, `fltp`, exactly 1,536,000 samples, and
  `pcmAccessUnitPairing=PASS`. The access units contain dependent streams and
  `pcmDependentSubstreamHandling=PASS` based on the retained eight-channel
  native decoder output.
- **Commands**:

  ```powershell
  cmake --build build-mf --target Eac3AccessUnitProbe --config Debug -- /m:1
  .\build-mf\Debug\Eac3AccessUnitProbe.exe `
    'media\03. iPad.m4a' --max-units 1000 --pcm
  .\build-mf\Debug\Eac3AccessUnitProbe.exe `
    'media\POWDER SNOW Live V9.8.6.eb3' --max-units 1000 --pcm
  ```

- **Next gate**: Implement the 64-band/640-tap QMF analysis and synthesis
  path independently of JOC and measure delay-aligned reconstruction SNR.

## Status refresh: 2026-08-24 (Gate 4 QMF reconstruction passed)

- **Probe**: Added an independent 64-band, 640-tap QMF analysis/synthesis
  diagnostic. It loads the user's local `prot64[640]` reference table at
  runtime, keeps analysis/synthesis delay state across 64-sample slots, checks
  finite output and arbitrary input split boundaries, and does not touch the
  production playback path.
- **Specification resolution**: TS 103 420 Pseudocode 12 correctly uses the
  analysis phase `(j - 0.5)`. On the next page, however, the synthesis matrix
  `N` equation expands to `(2*j - 4*n + 1)`, while Pseudocode 14 prints the
  conflicting `(2*j - 2*n - 1)`. The matrix equation is corroborated by ETSI
  TS 103 190-1 Pseudocode 66 (`2*j - 255` for `n = 64`). The official AC-4
  archive's 640 QWIN values compare exactly equal to the local TS 103 420
  `prot64` values (`maxAbsDiff=0`), so the probe follows the matrix equation.
- **Result**: `probeResult=PASS`. Delay-aligned reconstruction is impulse
  73.10 dB, sine 83.61 dB, sweep 78.24 dB, and deterministic random 78.30 dB.
  Every fixture reports the same 577-sample delay and unity gain within
  0.001. Silence remains exact zero, the output contains no subnormal samples,
  and arbitrary split boundaries plus explicit state reset are bit-identical
  to their single-call/fresh-instance references.
- **Acceptance adjustment**: The earlier 80 dB planning value was not a
  requirement from either ETSI document and exceeded the official window's
  measured 73.10 dB impulse response. The regression floor is now 72 dB for
  every non-silence fixture, while exact delay, gain, silence, state, and
  boundary invariants guard against accepting a phase-shifted approximation.
- **Command/evidence**:

  ```powershell
  cmake --build build-mf --target Eac3QmfProbe --config Debug -- /m:1
  .\build-mf\Debug\Eac3QmfProbe.exe `
    --table 'docs\dev\ts_103420_tables.c'
  ```

- **Next gate**: Parse JOC side information and reconstruct the complex object
  matrix while keeping this QMF path isolated from production playback.
- **Luna handoff**: Gate 5 is split into reviewed 5A syntax/Huffman, 5B pure
  coefficient mathematics, and 5C object-QMF reconstruction changes. Luna's
  next task is Gate 5A only; the scoped prompt and stop conditions are in
  `docs/dev/eac3-joc-gate5-luna-handoff.md`.

## Status refresh: 2026-08-24 (Gate 2 EMDF extraction)

- **Probe**: Extended the standalone diagnostic with a bounded Annex H EMDF
  parser. It scans each assembled syncframe at bit precision, validates
  `0x5838` plus the container length, decodes variable-bit fields and payload
  configuration, and preserves raw payload bytes. It does not interpret OAMD
  or JOC payload contents yet.
- **Apple Music M4A result**: The first 100 access units contained 100 valid
  EMDF containers. Each had payload 11 (OAMD, 67 bytes) and payload 14 (JOC,
  134 bytes), plus payloads 1 and 2; both target payloads occurred once per
  frame and `targetPayloadContainerPlacement=PASS`.
- **Raw EB3 result**: The first 100 access units contained 100 valid EMDF
  containers on the dependent stream. Each had payload 11 (67 bytes) and
  payload 14 (300 bytes), plus payloads 1 and 2; placement on the last
  applicable dependent stream passed.
- **Result**: Both samples returned
  `emdfResult=PASS stage=gate2-emdf-payload-extraction`. Payload contents and
  JOC sequence-count semantics remain for later gates.
- **Commands**:

  ```powershell
  cmake --build build-mf --target Eac3AccessUnitProbe --config Debug -- /m:1
  .\build-mf\Debug\Eac3AccessUnitProbe.exe `
    'media\03. iPad.m4a' --max-units 100 --emdf
  .\build-mf\Debug\Eac3AccessUnitProbe.exe `
    'media\POWDER SNOW Live V9.8.6.eb3' --max-units 100 --emdf
  ```

- **Next gate**: Pair the extracted target payloads with unmodified native
  FFmpeg PCM and verify dependent-substream handling before QMF work.

## Status refresh: 2026-08-24 (Gate 1 access-unit assembler)

- **Probe**: Added standalone `Eac3AccessUnitProbe`. It parses bounded AC-3
  and E-AC-3 syncframes, records absolute bit limits, frame type, substream
  ID, block count, sample rate, and original compressed bytes, then groups
  represented streams until each contributes six blocks. Production playback
  code remains unchanged.
- **Important raw-stream topology**: The raw
  `media\POWDER SNOW Live V9.8.6.eb3` sample uses one 2560-byte
  `AC3_CONVERT` core frame plus one 4096-byte dependent E-AC-3 frame per
  6656-byte access unit. Treating every `bsid` below 8 as invalid would have
  rejected this valid core-carriage form; the assembler now follows the
  AC-3-convert branch required by the documented frame-type gate.
- **M4A result**: Two runs with `--max-units 100` produced 100 units and 100
  independent syncframes each time. Every unit was 3072 bytes, 48 kHz, and
  1536 samples: `everyAccessUnit1536At48000=PASS`.
- **Raw EB3 result**: Two runs with `--max-units 100` produced 100 units and
  200 syncframes each time: 100 `AC3_CONVERT` plus 100 dependent E-AC-3
  frames. Every unit was 6656 bytes, 48 kHz, and 1536 samples:
  `everyAccessUnit1536At48000=PASS`.
- **Malformed-input checks**: `--self-test` passes valid-frame,
  truncated-frame, reserved-stream-type, block-count-mismatch, and
  sample-rate-change cases without out-of-bounds reads.
- **Commands**:

  ```powershell
  cmake --build build-mf --target Eac3AccessUnitProbe --config Debug -- /m:1
  .\build-mf\Debug\Eac3AccessUnitProbe.exe dummy --self-test
  .\build-mf\Debug\Eac3AccessUnitProbe.exe `
    'media\03. iPad.m4a' --max-units 100
  .\build-mf\Debug\Eac3AccessUnitProbe.exe `
    'media\POWDER SNOW Live V9.8.6.eb3' --max-units 100
  ```

- **Next gate**: Keep the assembled compressed bytes and access-unit timing
  as the input to Gate 2 EMDF payload 11/14 extraction.

## Status refresh: 2026-08-24 (Gate 0 packet/PCM correlation)

- **Probe**: Added standalone `Eac3Gate0Probe`. It inspects each demuxed
  E-AC-3 packet before unref, parses syncframe headers with the local framing
  code, and sends the same packet to libavcodec. No production decoder,
  Media Foundation, WASAPI, or JOC code is changed.
- **Apple Music M4A result**: On `media\03. iPad.m4a`, the first 100 audio
  packets were each 3072 bytes and contained exactly one `st0/sid0` syncframe
  with six blocks (100 independent syncframes, 0 dependent syncframes). All
  100 packet durations were 1536 samples at 48 kHz, so
  `packetEqualsComplete1536AccessUnit=PASS`.
- **Native PCM result**: libavcodec opened profile 30 E-AC-3 as 48 kHz,
  six-channel `5.1(side)`, planar float. It returned 151,408 samples from
  153,600 packet samples after the first packet's 2,192 `Skip Samples`
  side-data value; `nativePcmSampleCountAfterPacketTrim=PASS_WITH_CODEC_PRIMING`.
  The 99-frame versus 100-packet count is therefore codec priming/delay, not
  evidence of a packet syntax failure. Exact packet-to-frame one-to-one
  mapping remains intentionally inconclusive.
- **Command and evidence**:

  ```powershell
  cmake --build build-mf --target Eac3Gate0Probe --config Debug -- /m:1
  .\build-mf\Debug\Eac3Gate0Probe.exe `
    'media\03. iPad.m4a' --max-packets 100
  ```

  The run completed with `probeResult=PASS`; the generated report is local
  evidence only (`build-mf\eac3-gate0-apple-m4a.txt`) and is not committed.
- **Next gate**: Add the access-unit assembler contract and then isolate EMDF
  payload 11/14 extraction. Do not wire these diagnostics into production
  playback yet.

## Status refresh: 2026-08-24 (E-AC-3/JOC decoder plan)

- **Architecture decision**: The decoder plan now keeps FFmpeg/libavcodec for
  native E-AC-3 channel PCM and parses the same compressed packet through an
  AudioPlayer sidecar path for EMDF payload 11 (OAMD) and payload 14 (JOC).
  TS 103 420 permits the decoded time-domain channel downmix to enter the
  normative QMF analysis bank, so an FFmpeg pre-IMDCT hook is not the initial
  design.
- **Implementation boundary**: `docs/dev/eac3-joc-decoder-plan.md` defines
  eight gates from baseline discovery through renderer-neutral
  object PCM and OAMD. Windows dynamic spatial objects and Dolby Atmos for
  Headphones rendering begin after that contract.
- **First gate**: Extend the diagnostic probe to correlate the first 100 M4A
  packets, compressed syncframes/access units, and native decoded AVFrames.
  Do not modify production playback workers until packet/metadata/PCM/QMF/JOC
  alignment is independently demonstrated.
- **Unproven risk**: FFmpeg recognizes the Atmos profile and decodes audible
  core PCM, but complete dependent-substream channel output and EMDF alignment
  remain unverified. Patch FFmpeg only if the packet/PCM gate produces a
  reproducible missing-channel case.

## Status refresh: 2026-08-24 (PCM spatial sink and self-written parser)

- **Windows spatial sink probe**: Added diagnostic-only `SpatialBedProbe`. It
  generates a low-level synthetic 5.1.2 static object bed and submits eight
  mono float objects through `ISpatialAudioObjectRenderStream`; it does not
  use FFmpeg or Media Foundation decoding.
- **Local result**: On the current default `扬声器 (WALKMAN)` endpoint with
  Dolby Atmos for Headphones selected, the probe reported
  `spatialStreamAvailable=1`, native static mask `0xffffe`, a 48 kHz/32-bit
  mono object format, and `probeResult=PASS` after submitting 72,480 frames.
  A synchronized `WasapiLoopbackCapture` run captured 144,000 frames with
  `maxPeak=0.658183`, `blockRmsMax=0.276718`, and no dropout candidates.
  The harness classified the run as **INCONCLUSIVE** because its generic
  transient detector is not a listening test; the non-silent loopback proves
  that PCM reached the endpoint.
- **Self-written decoder first stage**: Added diagnostic-only
  `AtmosJocProbe`, which parses ETSI E-AC-3 syncframes without linking FFmpeg
  or another decoder implementation. On
  `media\POWDER SNOW Live V9.8.6.eb3`, `--max-frames 5000` parsed 5,000
  frames at 48 kHz, including 4,969 dependent frames — **PASS** for framing
  and stream-topology discovery, not yet PCM decode.
- **Decision**: The next decoder stage can target PCM output only and feed the
  already-working spatial sink. Exact JOC object metadata preservation remains
  a later stage; this probe separates decoder work from the previously failed
  compressed Media Foundation input path.

## Status refresh: 2026-08-24

- **Scope**: `MfAtmosProbe` is a diagnostic-only C++ target. Given an `.mka`
  sidecar, it uses `MFCreateSourceReaderFromURL`, obtains the native audio
  `MF_MT_SUBTYPE`, and reads one compressed sample. It does not create a Media
  Session, select an endpoint, decode to PCM, or modify normal playback.
- **Raw-stream preservation**: `scripts\test-mf-atmos.ps1` keeps the existing
  FFmpeg remux policy for `.eb3`/`.ec3` (`eac3`) and `.mlp` (`truehd`): stream
  copy into a tight-cluster Matroska sidecar, then invoke the probe. The
  original raw input is not decoded or rewritten.
- **Local results**:
  - `media\POWDER SNOW Live V9.8.6.eb3` remuxed under
    `build-mf\mf-atmos-probe\` and returned
    `nativeSubtypeName=DolbyDigitalPlus`, 8 channels, 48000 Hz, first
    compressed sample 6656 bytes — **PASS**.
  - `media\POWDER SNOW Live V9.8.6.mlp` remuxed under
    `build-mf\mf-atmos-probe\` and returned
    `nativeSubtypeName=DolbyTrueHD`, 8 channels, 48000 Hz, first compressed
    sample 480 bytes — **PASS**.
- **Validation**:
  - `cmake --build build-mf --target MfAtmosProbe --config Debug -- /m:1` —
    **PASS**.
  - `scripts\test-mf-atmos.ps1 -Source "media\POWDER SNOW Live V9.8.6.eb3"
    -BuildDir build-mf -Configuration Debug -NoBuild` — **PASS**.
  - `scripts\test-mf-atmos.ps1 -Source "media\POWDER SNOW Live V9.8.6.mlp"
    -BuildDir build-mf -Configuration Debug -NoBuild` — **PASS**.
  - `cmake --build build-mf --target AudioPlayerTests --config Debug -- /m:1`,
    then `ctest --test-dir build-mf -C Debug --output-on-failure` with the Qt
    bin directory on `PATH` — **PASS** (1/1).
- **Evidence limit**: This proves the current Windows MF source reader accepts
  these remuxed sidecars and exposes the expected compressed subtype. It does
  not prove Atmos metadata is rendered, that a receiver receives a bitstream,
  or that endpoint output is correct.

## Status refresh: 2026-08-24 (headphone renderer startup)

- **Renderer test path**: `MfAtmosProbe` now accepts `--render-ms`. After the
  compressed-type probe passes, it creates an MFPlay instance with the
  free-threaded callback option. MFPlay uses the Media Foundation Streaming
  Audio Renderer on the system default endpoint; it is a diagnostic path and
  does not alter the app's WASAPI/ASIO playback route.
- **Local result**: With the current default endpoint configured by the user as
  Dolby Atmos for Headphones, the command below returned
  `rendererStarted=PASS` and `rendererResult=PASS` after 8000 ms:

  ```powershell
  scripts\test-mf-atmos.ps1 -Source "media\POWDER SNOW Live V9.8.6.eb3" `
    -BuildDir build-mf -Configuration Debug -NoBuild -RenderMilliseconds 8000
  ```

- **Evidence limit**: The callback proves MFPlay entered the playing state and
  held the default renderer for the requested interval. It is not an acoustic
  verification of the binaural image; that needs a user listening observation.

## Status refresh: 2026-08-24 (MFPlay endpoint-output comparison)

- **Endpoint/session checks**: The diagnostic now records the Windows default
  render endpoint ID and MFPlay volume state. The selected endpoint maps to
  `扬声器 (WALKMAN)`; MFPlay reported volume `1.0` and `rendererMuted=0`.
- **EAC3 endpoint result**: A 6000 ms MFPlay run of the EAC3 MKA while a
  WASAPI loopback capture was attached to that endpoint produced `0` captured
  frames and `0` packets. The MFPlay state callback still returned
  `rendererStarted=PASS` / `rendererResult=PASS`.
- **WAV control result**: Under the identical MFPlay/default-endpoint/loopback
  setup, `build-mm\fixtures\smoke.wav` produced 162720 captured frames, 339
  packets, peak `0.303518`, and RMS `0.216013`. The renderer and endpoint are
  therefore functional.
- **Conclusion**: On this Windows installation, MFPlay accepting the remuxed
  EAC3 source and reporting a playing state does not yield audible endpoint
  output. Do not use MFPlay as the app's Dolby headphone/stereo renderer.
  This is a concrete failure of this renderer path, not proof that all lower
  level Media Foundation topologies are impossible.

## Status refresh: 2026-08-24 (modern Media Engine probe)

- **Modern path**: Added diagnostic-only `MfMediaEngineProbe`, using the native
  `IMFMediaEngine` interface that underlies the modern Windows `MediaPlayer`
  stack. It selects the default console render endpoint and does not touch the
  production Qt playback route.
- **WAV control**: `MfMediaEngineProbe build-mm\fixtures\smoke.wav
  --render-ms 3000` returned `engineReady=PASS`, `enginePlaying=PASS`, and
  `engineRenderResult=PASS`. With the same endpoint, WASAPI loopback captured
  162720 frames, 339 packets, peak `0.303518`, and RMS `0.216013` under
  `build-mf\engine-loopback-wav\report.json` — **PASS** at the endpoint-output
  evidence layer.
- **E-AC-3 sidecar**: `scripts\test-mf-atmos.ps1 -Source "media\POWDER SNOW
  Live V9.8.6.eb3" -BuildDir build-mf -Configuration Debug -NoBuild
  -MediaEngine` reached `engineLoad=PASS` but returned
  `engineReady=FAIL lastEvent=5 errorCode=4 extendedHresult=0xc00d5212` —
  **FAIL** for this machine's native Media Engine source/decode path.
- **TrueHD sidecar**: The same probe returned the same `engineReady=FAIL`
  result — **FAIL** for the tested native Media Engine path.
- **Interpretation**: The modern engine is a valid AVFoundation-like Windows
  API and its WAV render path works, but this installation does not advertise
  or resolve the tested E-AC-3/TrueHD sources through `IMFMediaEngine`. This is
  a stronger negative result than the earlier MFPlay test, while still not
  proving that a packaged WinRT `MediaPlayer` wrapper or another container
  changes the codec availability. Keep the failure isolated to the diagnostic
  path and do not route normal playback to it.

## Status refresh: 2026-08-24 (Apple Music Atmos M4A)

- **File identification**: `media\03. iPad.m4a` is a normal M4A container with
  an `ec-3` audio track reported as `Dolby Digital Plus + Dolby Atmos`, 48 kHz,
  6 channels, 768 kbps, and 202.462333 seconds. `ffprobe` completed without an
  encryption or demux error.
- **Modern engine**: Direct `MfMediaEngineProbe` reached `engineLoad=PASS` but
  returned `engineReady=FAIL lastEvent=5 errorCode=4
  extendedHresult=0xc00d5212` — **FAIL**.
- **MFPlay control**: Direct `MfAtmosProbe` recognized the native Dolby Digital
  Plus subtype and returned `rendererStarted=PASS`, but the paired WASAPI
  loopback report under `build-mf\apple-m4a-mfplay-loopback\report.json`
  captured 0 frames and 0 packets — **FAIL** at the endpoint-output layer.
- **FFmpeg control**: `ffmpeg -v error -i "media\03. iPad.m4a" -t 5 -map
  0:a:0 -f null NUL` completed with exit code 0 — **PASS** for decoding.
- **Conclusion**: The Apple Music Atmos file is a useful real-world test
  sample, but it does not change the current Windows MF result. Keep FFmpeg
  as the decoder; the Windows Atmos-for-Headphones setting cannot compensate
  for the missing MF E-AC-3 decode stage.

## Status refresh: 2026-08-24 (compressed Atmos endpoint capability)

- **Endpoint capability query**: `MfMediaEngineProbe` now also queries the
  current default `扬声器 (WALKMAN)` endpoint with Windows' enhanced media
  capability strings. `audio-endpoint-codec=DD+JOC` and `DD+` both returned
  `NotSupported`; the PCM2.0/5.1/7.1 queries returned `Maybe`.
- **WASAPI encoded-format query**: The probe constructed
  `WAVEFORMATEXTENSIBLE_IEC61937` for both
  `KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL_PLUS` and the dedicated
  `KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL_PLUS_ATMOS` subtype. For the
  current endpoint, shared mode returned `S_FALSE` (no exact supported format)
  and exclusive mode returned `0x88890008` (`AUDCLNT_E_UNSUPPORTED_FORMAT`) for
  both formats.
- **Interpretation**: This endpoint cannot accept compressed E-AC-3/JOC frames
  from an application through its exposed WASAPI format. This is not an HDMI
  playback claim; it is a capability result for the selected USB headphone
  endpoint. Windows Spatial Audio metadata APIs remain a PCM spatial-object
  interface and do not define a public way to submit a Dolby E-AC-3 JOC frame.
- **Next decision**: A direct metadata-preserving route is viable only if the
  user selects an endpoint whose capability query reports `DD+JOC` support, or
  if a packaged Windows media app can use a licensed system Dolby decoder that
  is not exposed to this endpoint. For the current endpoint, keep the
  compressed stream path unimplemented and preserve FFmpeg for audible PCM
  playback.

## Current focus

1. Complete Gate 0 in `docs/dev/eac3-joc-decoder-plan.md`: correlate compressed
   M4A packets, complete 1,536-sample access units, dependent substreams, and
   native FFmpeg AVFrames.
2. Extract frame-aligned EMDF payloads before implementing QMF or JOC.
3. Keep production playback and the existing static spatial bed unchanged
   until the standalone decoder reaches the renderer-neutral output contract.
## Status refresh: 2026-08-27 (Renderer R2B1 CPU BRIR convolution; bounded PASS)

- **Implementation**: Independent scalar radix-2 FFT and uniform partitioned
  BRIR convolver. Default P=1024/F=2048/K=16, with configurable short
  synthetic sizes for oracle tests. The fixed-block core uses caller buffers;
  the offline stream wrapper handles arbitrary splits, zero padding, and exact
  `inputFrames + IRLength - 1` tail cropping.
- **Validation**: `Eac3BrirConvolverProbe` synthetic self-test passes 17 cases;
  real cache integration passes 20 cases, including M+000 and M+090 impulse
  comparison against both cache ears. Direct oracle, three split patterns,
  22-speaker sum, silence, reset, idempotent finish, nonfinite input, and
  nonzero delay rejection are covered.
- **Focused commands**: `build-mm\Debug\Eac3BrirConvolverProbe.exe
  --self-test` runs the 17 synthetic cases; passing
  `tmp\r2a-system-h-brir.cache` runs those plus the three real-cache cases.
- **Boundary**: only finite nonnegative integer/all-zero `Data.Delay` is
  accepted in this slice; fractional/nonzero delay is `R2B2` pending. No
  normalization, limiter, DRC, decoder/object PCM, WASAPI, FFmpeg, or CUDA.
- **Full validation evidence**: `scripts\validate-all.ps1 -BuildDir build-mm
  -Configuration Debug` passed unit tests, report schema, and smoke. Aggregate
  report: `build-mm\validation-report.json`; smoke log/report:
  `build-mm\cache\logs\player-smoke-20260827-182206-335-1eed6d25.log` and
  `player-smoke-20260827-182206-335-1eed6d25.report.json`.
