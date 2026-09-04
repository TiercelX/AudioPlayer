# Legacy optional Windows Spatial comparison integration (Gate 8W)

Gate 7C2 proves that the current decoder can submit real 15-object-plus-LFE
programs to Windows Spatial Audio. Gate 7D proves non-silent stereo endpoint
output through synchronized loopback. The user accepted the basic rendered
result as audible and generally good, then reported in more focused listening
that LFE was too strong, the result was muddy, and the stage felt close. That
feedback exposed a renderer gain mismatch rather than completing tonal or
localization acceptance. Controlled A/B and binaural localization accuracy
remain open. This page preserves the legacy optional Windows Spatial Audio
comparison plan only; it no longer defines the production completion route.

## Windows convenience entry

For a safe single-command or double-click entry, use
scripts/run-atmos-windows.ps1. Its default mode is Validate, which checks the
selected raw/container input and local dependencies without submitting audio
or claiming endpoint capability:

    scripts/run-atmos-windows.ps1 -InputPath media\01.m4a -Mode Validate
    scripts/run-atmos-windows.ps1 -InputPath media\01.m4a -Mode Render -BearProfile OfficialMain -OpenOutput
    scripts/run-atmos-windows.ps1 -InputPath media\01.m4a -Mode Render -BearProfile SystemHV6
    scripts/run-atmos-windows.ps1 -InputPath media\01.m4a -Mode Spatial
    scripts/run-atmos-windows.ps1 -InputPath media\01.m4a -Mode Spatial -DisableLfe

With no InputPath, the PowerShell entry opens a Windows Forms picker. The
picker and command-line validation accept only .m4a, .mp4, .mka, .mkv, .eb3,
.ec3, and .eac3; ADM WAV and arbitrary WAV are intentionally rejected. Each
invocation creates a new tmp\windows-atmos\<stem>-<mode>-<UTC timestamp>
directory, so an old evidence directory is not overwritten.
scripts/render-atmos-windows.cmd and scripts/play-atmos-windows-spatial.cmd
are thin drag-and-drop launchers.

## HRTF capability probe (diagnostic only)

The separate `SpatialDynamicProbe` can exercise the official Windows HRTF
interface without BEAR or JOC input:

    build-mm\Release\SpatialDynamicProbe.exe --renderer hrtf --hrtf-environment small --duration-ms 1500 --impulse-delay-ms 300 --objects 1 --signal impulse --position front
    build-mm\Release\SpatialDynamicProbe.exe --renderer hrtf --hrtf-environment outdoors --duration-ms 1500 --impulse-delay-ms 300 --objects 1 --signal impulse --position front

This path uses one mono float32/48 kHz dynamic object, explicit omni
directivity, identity orientation, and the selected activation environment.
It deliberately leaves `DistanceDecay` null (`activation-null-default`) rather
than applying a guessed JOC/BEAR distance law. The standard renderer remains
the default. The first Small/Outdoors front impulse captures and their
onset-aligned comparison are recorded in the HRTF section of the E-AC-3/JOC
status tracker. This is a capability and signal-statistics seam only, not
production JOC integration, Dolby equivalence, physical headphone evidence,
or a listening quality claim.

## Gate7C experimental HRTF backend

The real JOC/BEAR diagnostic delegate also accepts
`-SpatialRenderer standard|hrtf` and `-HrtfEnvironment small|outdoors`:

    scripts/play-atmos-spatial.ps1 -InputPath tmp\riptide-spatial-audit-20260901\Riptide.ec3 -MaxAccessUnits 500 -DisableLfe -PositionRadiusMode unit -SpatialRenderer hrtf -HrtfEnvironment small
    scripts/audit-atmos-spatial.ps1 -InputPath tmp\riptide-spatial-audit-20260901\Riptide.ec3 -MaxAccessUnits 500 -DisableLfe -PositionRadiusMode unit -SpatialRenderer hrtf -HrtfEnvironment outdoors -Submit

This is an opt-in diagnostic backend; `standard` remains byte/behavior
compatible and the production default. HRTF uses the same 15 decoded Gate6C
object streams and timebase, `StaticObjectTypeMask=None`, omni directivity,
identity orientation, explicit environment, and null `DistanceDecay`. The
evaluated object volume (including the existing -15 dB headroom) is applied
to each object PCM sample before submission, with finite and overflow checks.
It does not call `SetGain`, custom decay, or submit a static LFE object, and
therefore requires explicit `-DisableLfe`; missing that flag is a validation
error rather than an implicit LFE drop.

The bounded Riptide Small/Outdoors runs and normalized per-channel power
comparisons are recorded in the status tracker under
`tmp\gate7c-hrtf-ab-20260901\`. Both endpoint submissions and loopbacks were
non-silent, but the audit result remains
`INCONCLUSIVE_ENDPOINT_OUTPUT_NOT_PROVEN`: these data establish a capability
and controlled signal-statistics seam only, not HRTF correctness, Dolby
equivalence, production suitability, or subjective listening quality.

For comparing these HRTF captures with another Spatial endpoint capture, use
the semantic endpoint-to-endpoint comparator
`scripts\compare_spatial_endpoint_loopback.py`, not the Object Direct
comparator. It records candidate/reference labels, renderer and environment,
independently aligns each capture's first active sample, and reports
per-channel RMS, normalized correlation, and normalized per-channel power
fractions over 0--20 kHz. The existing Riptide reports are
`tmp\gate7c-hrtf-ab-20260901\comparisons\small-vs-standard-unit.json`,
`outdoors-vs-standard-unit.json`, and `small-vs-outdoors.json`. They show
signal-statistics comparisons between these particular endpoint captures;
they do not prove Small/Outdoors equivalence, HRTF correctness, or listening
quality. Object Direct remains a separate renderer-neutral diagnostic and is
not a reference for endpoint-to-endpoint claims.

Extension acceptance does not promise that every vendor EB3 framing is
accepted by the native Gate6C parser. For the local MONTERO.eb3 sample,
lossless ffmpeg packet copy to -f eac3 preserved the file byte-for-byte,
including its observed 16-byte per-access-unit wrapper, and Gate6C still
rejected packet 1. No automatic normalization is claimed or applied until a
format-specific, independently validated unwrapping rule exists.

For the comparable native path, the corrected unified delegate leaves the
BEAR source/import arguments implicit so the export wrapper selects its
verified main `build-visr-bear-6\python\Release` binding path. A bounded
`media\MONTERO.ec3` SystemHV6 run at 157 access units passed and produced
finite stereo output; evidence is under
`tmp\windows-atmos-system-h-v6-render-ec3-default-import\`. This does not
make MONTERO.eb3 interchangeable: its full audit found 5,311 repeated
6,656-byte payload AUs, each containing a 2,560-byte legacy AC-3 frame and a
4,096-byte dependent E-AC-3 frame behind a 16-byte wrapper. The wrapper
variant changes after AU 82, its carriage metadata is not a continuous BCD
timecode, and the native probe rejects the variant at the second wrapper.
The audit is `tmp\eb3-structure-audit\montero-eb3-structure.json`; no
automatic EB3 unwrapping is enabled.

OfficialMain is the pinned BEAR main checkout
6127e897b941211051c2ad135ee09b00be2e6ae0 with the official default_v1.1.tf
(SHA-256 171acae2159e60ffe9d705abc16a79be129ecd06d37186fae413a265b6ed71e8).
SystemHV6 is an explicit offline experiment using the v6 TensorFile
system-h-22-v6.tf (SHA-256
8195b0b456f9172a709375f9da8e2a39c63d9d5f7b03aa02ee90359a8bffe7c9), the
22-channel 9+10+3 layout with missing B+135/B-135. It is not the default
24-channel data and is not a Windows Spatial Audio input. The helper does not
alter the production default or global PATH.

Mode Spatial delegates to the existing Gate 7C diagnostic path. It submits
real decoded object data to ISpatialAudioObjectRenderStream, is not a BEAR
render, and produces no audio file. Its result remains endpoint-submission
evidence; synchronized loopback or manual listening is required for endpoint
output evidence. Mode Validate deliberately does not call the spatial probe
because the current probe submits generated audio.

`-DisableLfe` is available only in Mode Spatial. It keeps the static LFE object
and its buffer/metrics in the Gate7C transaction, but submits it at volume
`0.0` (`lfePolicy=DISABLED_BY_USER`). The default remains
`lfePolicy=PROGRAM_HEADROOM` with volume `0.177827941`; dynamic objects retain
the `-15 dB` program gain. The report's `dynamicGainHeadroomDb=15` records the
positive headroom amount; all position/ramp/queue behavior is unchanged.
This is an endpoint-submission A/B control, not a claim that the endpoint's
internal bass management is bypassed.

For a diagnostic position-radius A/B, pass
`-PositionRadiusMode source` (the default) or `-PositionRadiusMode unit` to
`scripts\play-atmos-spatial.ps1`; `scripts\audit-atmos-spatial.ps1` forwards
the same parameter to its delegate. `unit` keeps the listener-relative source
direction but normalizes each nonzero position to a 1 metre radius. A zero
vector remains zero, with no guessed direction. Gate7B's text report records
the mode and source/output radius min/max/mean/sample count. This option does
not change PCM, JOC metadata, gain/headroom, LFE, ramps, or queue behavior;
it is a diagnostic endpoint-submission A/B and is not the production default.
It does not establish HRTF/localization quality or subjective listening
equivalence.

For a second, orthogonal diagnostic isolation, pass
`-PositionDirectionMode metadata` (the default) or
`-PositionDirectionMode front` to `scripts\play-atmos-spatial.ps1`;
`scripts\audit-atmos-spatial.ps1` forwards the same parameter. `front`
keeps the selected source/unit radius for each nonzero object but replaces
its direction with the Windows listener-relative front axis
`(0,0,-radius)`. A zero vector remains zero. The native Gate7B/7C logs and
provenance record the selected direction mode and output ranges. This
changes position metadata only: PCM, JOC gain, headroom, LFE policy, object
count, ramps, and queue behavior are unchanged. It is a diagnostic
metadata-vs-fixed-front A/B, not a production default or evidence of
HRTF/localization or listening equivalence. For equal-radius comparison,
combine `-PositionRadiusMode unit -PositionDirectionMode front`.

A single authorized bounded live `unit+front` run used the same Riptide input,
500 AU, LFE off, Release build, and endpoint as the clean earlier
`unit+metadata` capture. The Gate7B target was `X=0,Y=0,Z=-1` with 7,500
samples; Gate6C/7B/7C and the loopback signal analyzer passed, while the
overall wrapper remained `INCONCLUSIVE_ENDPOINT_OUTPUT_NOT_PROVEN` by
contract. Evidence is under
`tmp\spatial-direction-ab-20260901\unit-front\Riptide-20260901T045322239Z\`;
the machine-readable A/B summary is
`tmp\spatial-direction-ab-20260901\comparisons\unit-front-vs-unit-metadata-summary.json`
with a Markdown companion. The comparison is independently onset-aligned and
reports normalized 0--20 kHz per-channel fractions; it is an endpoint
loopback direction-isolation observation only. Because startup and endpoint
state are independent, the measured differences are not proof of a pure
direction cause, listening improvement, HRTF equivalence, or headphone
quality.

For a candidate non-EQ horizontal focusing A/B, pass `-AzimuthFocus N` with
`0 <= N <= 1` to `scripts\play-atmos-spatial.ps1` (also forwarded by
`scripts\audit-atmos-spatial.ps1` and the unified Windows entry). The default
`0` is identity. In metadata direction mode, `N` linearly reduces horizontal
azimuth toward front while retaining the horizontal radius and Windows
vertical coordinate: `theta=atan2(x,-z)`, `theta'=(1-N)*theta`, then
`x=h*sin(theta')`, `z=-h*cos(theta')`. `N=1` therefore focuses only the
horizontal component and retains elevation/total radius. Zero and horizontal-
zero vectors remain finite and unchanged. Explicit `PositionDirectionMode
front` is the final fixed-front override and ignores focus direction. Native
logs/provenance record the focus and ranges. This changes position metadata
only; it does not change PCM, gain, headroom, LFE, object count, ramps, or
queue behavior. It is a diagnostic candidate, not a production default or
listening-quality/HRTF claim.

A user-authorized bounded live A/B was run on the matching Riptide EC-3
reference with 500 access units and LFE disabled. The first source capture was
contaminated and is explicitly superseded. The clean source-repeat evidence
is under
`tmp\spatial-radius-ab-20260901\source-repeat\Riptide-20260901T043309251Z\`;
the existing unit evidence is under
`tmp\spatial-radius-ab-20260901\unit\Riptide-20260901T042854793Z\`.
Both passed Gate6C/7B/7C on the same endpoint. Corrected comparison output is
`tmp\spatial-radius-ab-20260901\comparisons\source-repeat-vs-unit-summary.json`.

The corrected pair agrees closely: unit/source-repeat RMS ratio `0.998736`
(`-0.011 dB`) and peak ratio `0.999779` (`-0.002 dB`); 0--80 Hz normalized
fraction `0.008819 -> 0.008632`, 200--375 Hz `0.540866 -> 0.540902`.
Loopback was non-silent and unclipped, but the wrapper remains
`INCONCLUSIVE_ENDPOINT_OUTPUT_NOT_PROVEN`; independent onset values are not
a latency measurement. The corrected evidence does not support a measurable
radius-dependent spectral/level effect or a listening-quality conclusion.

The primary route is the application-owned self-renderer tracked in
`eac3-joc-next-roadmap.md`: I0P/I0 raw EB3 to System H/BRIR stereo WAV, I1
ordinary stereo WASAPI lifecycle, I2 native M4A/trim/seek, and I3 production
and release acceptance. It uses no FFmpeg/libav dependency in the Atmos
runtime and does not depend on a Dolby/private endpoint renderer. Nothing in
this Gate 8W page can complete or block I0-I3.

## Gate 8W program-headroom invariant

This headroom applies only to the Windows object comparison bridge. It is not
an I0-I3 self-renderer gain, normalization target, or LFE policy.

Apply the same 15 dB program headroom to every submitted signal plane. Dynamic
objects combine that base headroom with their OAMD gain through `SetVolume()`;
the static LFE has no dynamic OAMD gain, so set it once to
`10^(-15/20) = 0.177827941` after activation. Leaving static LFE at the Windows
default volume of `1.0` makes it 15 dB louder relative to zero-gain dynamic
objects and invalidates subjective spatial comparisons. Do not change the
coordinate transform while evaluating this correction.

This is a common headroom offset, not a fixed final gain for every signal. Use
`10^((metadataGainDb - 15)/20)` for a dynamic object, so an OAMD gain of
`+15 dB` reaches `1.0`, `0 dB` reaches `0.177827941`, and lower metadata gains
remain lower. Future static bed channels decoded from the same program must use
the same base headroom as LFE. If no separate bed-channel gain metadata exists,
their volume is `0.177827941`; if such metadata exists, combine it with the
common offset using the same formula. Activate beds as their fixed Windows
`AudioObjectType` channels and do not call dynamic-only `SetPosition()` on them.

## Standards and Windows renderer boundary

Keep three metadata layers distinct:

- TS 103 420 OAMD/JOC supplies decoded object essence and time-aligned object
  properties. It does not prescribe the binaural HRTF, room response or reverb
  algorithm.
- TS 102 366 Annex H EMDF payload `0x7` is optional headphone-rendering data.
  It can carry pre-binaural channel/LFE gains, propagation delay, early and late
  BRIR coefficients, and RT60 values. Inventory it during candidate detection,
  but do not require it: neither supplied real sample contains payload `0x7`
  over its complete access-unit range.
- The selected-renderer `ISpatialAudioObject` contract accepts object PCM,
  dynamic position and volume, plus static channel identity. It has no public
  fields for the Annex-H BRIR/RT60 data or OAMD size, zone, snap and distance
  extensions. Do not silently approximate those properties or claim they were
  forwarded to Dolby Atmos for Headphones.

Media Foundation spatial samples do not remove this boundary. Their metadata
items use a decoder-defined format GUID that the active renderer must support;
the public Windows SDK does not define a Dolby metadata format that this custom
decoder can populate. The separate `ISpatialAudioObjectForHrtf` API offers
environment and distance controls only on the explicit Windows Sonic for
Headphones path and cannot be combined with Dolby Atmos for Headphones.

For the supplied samples, absence of payload `0x7` means the current Cavern
comparison is a renderer/geometry/loudness comparison, not evidence of an
omitted standardized headphone payload. A future sample containing `0x7`
should first be inventoried and preserved diagnostically; using it would require
an independently implemented binaural renderer or a documented renderer-specific
metadata contract, not the current generic Dolby object stream.

## Legacy Gate 8W decision: reuse the ordinary-player control plane

FFmpeg/libav remains useful and is not removed. An optional Windows comparison
bridge may reuse or mirror these existing responsibilities:

- `PlaybackSourceService` probing, raw `.eb3`/`.ec3` remux sidecars, cache and
  duration ownership;
- libav demux and base E-AC-3 frame decode, retaining compressed `AVPacket`
  access so JOC/OAMD payloads are not lost;
- the decoder-worker session/generation pattern, bounded producer behavior,
  cancellation, error delivery, progress signals, and end-of-stream handling;
- the player `play`/`pause`/`stop`/`seek` state machine, output-device selection,
  volume setting, recovery boundaries, and teardown order.

Do not present this legacy comparison data plane as the primary self-renderer.
Once FFmpeg has reduced the source
to interleaved/downmixed PCM, the 15 object programs and their changing
positions/gains are gone. Replace that data plane with:

```text
container/raw sidecar
  -> libav packet + base E-AC-3 AVFrame pairing
  -> reusable Gate 1-6 JOC/OAMD decoder session
  -> Gate 6C Batch (15 planar objects + LFE + aligned metadata)
  -> Gate 7C bounded queue
  -> Windows Spatial Audio object render worker
  -> Dolby Atmos for Headphones / selected spatial endpoint
```

## Gate 8W routing and fallback

Do not route every `eac3` source to the object path and do not trust a UI Atmos
label alone. The supplied Apple and DME files both appear to FFprobe as E-AC-3
profile `30`; that field does not distinguish usable JOC.

For an E-AC-3 candidate, the decoder session must verify a bounded initial
prefix for the required EMDF/OAMD/JOC payloads and a supported configuration.
Only then select object mode. Before the spatial stream starts, any unsupported
or non-JOC source falls back to the current PCM pipeline without changing user
state. A deterministic failure after object playback starts is reported rather
than silently switching render models mid-track.

If retained, keep this as an explicitly diagnostic comparison mode adjacent to
`WindowsWasapiAudioPlayer`, not an automatic production route or new public
backend ID. Normal WASAPI/FFmpeg/ASIO/ALSA paths remain unchanged.

## Legacy Gate 8W-A: reusable comparison decoder session

Extract the real access-unit, EMDF, OAMD, JOC/QMF and Gate 6C orchestration from
`Eac3AccessUnitProbe` into a non-Qt or narrowly wrapped runtime component. The
probe becomes one caller of the same component rather than the production code
copying a second decoder.

The session API must support:

- open candidate and report `Joc`, `NotJoc`, `Unsupported`, or `Malformed`;
- sequential production of complete `eac3gate6c::Batch` values;
- bounded callback backpressure and cancellation;
- exact source/trim/output sample accounting and terminal flush;
- generation reset with no retained OAMD/JOC/QMF state;
- the same diagnostics used by Gates 5-7, without per-access-unit console spam.

Gate 8W-A does not touch the UI or play audio. Both existing 1,000-unit probe
results and self-tests must remain bit/count identical.

## Legacy Gate 8W-B: diagnostic Windows object workers

For a maintained comparison mode, promote the Gate 7C renderer/core from probe
ownership into Windows-backend-adjacent diagnostic classes. Keep two workers:

- decoder producer: owns libav and all Gate 1-6 state;
- spatial consumer: owns COM, endpoint, stream, event, notify, 15 dynamic
  objects and static LFE from activation through release.

Use session and generation IDs on every cross-thread callback. Preserve teardown
order: stop/release the spatial output, stop and join the decoder, then destroy
queue/session storage. Add structured logs for detection, prebuffer, first
commit, position, underrun, cancel, terminal commit and cleanup.

Treat a Windows spatial-format change as output invalidation, not as an in-place
renderer preset update. On `SPTLAUDCLNT_E_DESTROYED`,
`AUDCLNT_E_DEVICE_INVALIDATED`, or `AUDCLNT_E_RESOURCES_INVALIDATED`, pause
producer admission, stop and release the Spatial Audio stream and objects,
re-query the selected endpoint's static mask, dynamic capacity, and object
format, then rebuild and prebuffer the same live generation before resuming.
Do not silently fall back to ordinary stereo PCM while object playback is still
selected.

Gate 8W-B first exposes a backend-level test entry, not automatic UI routing. Its
acceptance is a full supplied file or a bounded duration with zero queue loss,
zero underrun, exact frame accounting, synchronized loopback and cancel/finish
stress.

## Legacy Gate 8W-C: basic comparison-mode behavior

Connect the object mode behind `WindowsWasapiAudioPlayer` for supported JOC
sources. Reuse the existing player state and signals, but implement object-mode
operations explicitly:

- play: create a fresh generation, detect/prebuffer, then start the spatial
  stream;
- pause/resume: stop/start spatial submission without advancing source time or
  discarding the queued generation;
- stop: cancel output first, cancel/join decoder second, clear queue third;
- volume: multiply Gate 7B object volume by the user scalar without changing
  object identity or metadata history;
- position: derive from committed source frames plus the playback start offset,
  not producer decode progress;
- natural EOS: commit the single terminal quantum before entering Stopped.

Basic Gate 8W-C may deliberately disable seeking and active output switching in
object mode until Gate 8W-D, but the UI must say so rather than performing an
incorrect PCM-style operation.

## Legacy Gate 8W-D: seek, switching and recovery

Seeking cannot simply issue FFmpeg `-ss` and reset the object decoder at an
arbitrary compressed frame: differential JOC/OAMD state may depend on earlier
units. Establish a verified random-access/preroll rule, decode preroll without
submission, rebase Gate 6C output to zero for the new generation, and start only
after all 15 initial properties are available.

Output-device/spatial-mode changes and invalidation use the existing high-level
WASAPI transaction boundary, but object mode always rebuilds the spatial stream
and advances generation. Never hot-swap COM objects across worker threads.

## Current evidence boundary

The Gate 7D DME loopback captured continuous non-silent stereo endpoint output
with zero dropout. The generic transient detector flagged two program regions,
while immediate human feedback accepted the overall result as sounding good.
Gate 7D therefore passes basic endpoint-output and subjective acceptance, with
the detector candidates and lack of controlled A/B retained as explicit limits.

## Bounded low-frequency and loopback diagnostics

`tools/atmos-render/object_direct_oracle.py` is a renderer-neutral diagnostic
for an existing Gate6C BSCN bundle. It validates the 15 planar object streams,
applies the current dynamic headroom and metadata gain/ramp, and sums the
objects to dual-mono stereo. It deliberately performs no HRTF, positioning,
Windows Spatial submission, or subjective listening test. LFE is excluded by
default; `--include-lfe --lfe-gain-db 0` is an explicit diagnostic A/B only.
Its gain ramp follows the native adapter: audible-to-audible ramps interpolate
in dB, while ramps with a silent endpoint interpolate linear amplitude; an
update interrupts any previous ramp at its evaluated current state.

Example:

```powershell
python tools/atmos-render/object_direct_oracle.py `
  --bundle tmp\riptide-spatial-audit-20260901\bundle `
  --output-dir tmp\riptide-spatial-audit-20260901\object-direct-riptide-full-v5-no-lfe
```

`scripts/audit-atmos-spatial.ps1` is a bounded, report-first orchestration
entry. It defaults to dry mode and cannot submit audio unless `-Submit` is
explicitly supplied. With `-Submit`, it starts `WasapiLoopbackCapture` first,
waits for its ready file, invokes `play-atmos-spatial.ps1` with the requested
bounded AU count and LFE policy, then writes process/endpoint/loopback sidecars
under a fresh timestamped directory. A clean loopback remains
`INCONCLUSIVE` under this repository's evidence contract; endpoint submission
or internal PCM alone is not endpoint-output proof. This route is diagnostic
and does not change production bed ownership or UI playback behavior. If a
post-start ready timeout or delegate exception occurs, the report is still
written and the script exits nonzero while preserving the specific result.

`tools/atmos-render/compare_object_direct.py` can produce a compact JSON and
Markdown comparison for two such reports. Keep short `--max-units 8` results
labelled as startup/decode smoke; they are not representative loudness or
quality measurements.

When loopback produces a WAV, `tools/atmos-render/analyze_loopback_wav.py`
uses a minimal RIFF/WAVE parser for PCM8/16/24/32, IEEE-float32/64, and
WAVE_FORMAT_EXTENSIBLE PCM/float capture formats. It reports channel
peak/RMS/crest, clipping, FFT band fractions, frame-based downmix statistics,
and conservative `-20/-40/-60 dB` threshold timings. If no transient is present it returns
`INCONCLUSIVE_NO_VALID_TRANSIENT`; a program tail is explicitly censored and
is not labelled a reverb decay. A nonzero or unparseable analyzer run causes
`audit-atmos-spatial.ps1` to report `INCONCLUSIVE_ANALYZER_FAILED` rather than
silently retaining an endpoint result.

For a dry synthetic endpoint probe, `SpatialDynamicProbe` supports
`--signal sine|impulse`, `--objects 1..15`, and fixed test positions
`--position moving|front|left|right|upper`. These options only change the
generated probe signal/position; they do not change the production renderer,
bed ownership, or acceptance evidence. A live probe still requires an
endpoint with sufficient dynamic-object capacity and must be labelled
endpoint-submission-only unless loopback and the applicable acceptance checks
are present.

## Offline JOC low-frequency matrix accounting

For an offline trace, use
`scripts/analyze_joc_low_frequency.py <trace.jsonl> --output-dir <dir>`.
The diagnostic Writer records whole QMF subbands 0 and 1 plus independent
time-domain LFE energy. At 48 kHz and 64 bands these are nominally 0--375 Hz
and 375--750 Hz; the trace cannot resolve <=80 Hz versus 80--200 Hz. It also
records maximum nonzero coefficients per row and multi-input row-timeslots,
with `abs(coefficient)>1e-12` as the explicit threshold. The analyzer reports
Qin energy, object actual/coherent/incoherent energy, cross-term energy,
actual/Qin and actual/incoherent ratios, and matrix row norms (theoretical
coherent-gain bounds for unit-input rows). In formulas,
`E_incoherent=sum(|M*Qin|^2)` and
`E_coherent=sum(|sum(M*Qin)|^2)`; their difference is the coherent cross term.
Object-energy totals are not an acoustic conservation or quality proof.

The reproducible 1000-AU Riptide report is
`tmp/riptide-spatial-audit-20260901/joc-lowfreq-riptide-1000-v2/` and the
same-length MONTERO control is
`tmp/montero-media-audit/joc-lowfreq-montero-1000-v2/`. Riptide has zero
independent LFE energy, `maxNonzeroCoefficientsPerRow=1`,
`multiInputRowTimeslots=0`, and `crossTerm=0`; this directly explains the
zero cross term for the audited rows. MONTERO has nonzero independent LFE,
`maxNonzeroCoefficientsPerRow=5`, `multiInputRowTimeslots=157486`, and a
`5680.06256` coherent matrix cross term. A zero reconstruction residual means
Qout is reproduced from the traced non-LFE Qin formula, not that upstream
folding, Dolby equivalence, endpoint output, or listening quality has been
proved.

For <=80 and 80--200 Hz, use the complementary
`scripts/compare_joc_time_domain_bands.py` with the same source and Gate6C BSCN
bundle. It verifies the 5.1(side) mapping (unknown layouts require explicit
indices), decodes exactly 1,536,000 frames here, and applies a 4096-frame
Hann/Welch analysis. Its Riptide and MONTERO JSON/Markdown reports are under
`tmp/riptide-spatial-audit-20260901/time-domain-riptide-1000-matching-fnv2/` and
`tmp/montero-media-audit/time-domain-montero-1000-matching-fnv2/`. The MONTERO
bundle in the latter report is provenance-bound to the same `media\\MONTERO.ec3`
input by absolute path and FNV-1a-64 digest. Missing or mismatched
`bundle-provenance.json` is rejected by default; the explicit
`--allow-unverified-bundle-source` escape hatch always labels the result
`INCONCLUSIVE_UNVERIFIED_BUNDLE_SOURCE`. This resolves finer
frequency boundaries: decoded-main/LFE/raw-object energies in <=80 Hz are
Riptide `1.038203/0/1.040006` and matching MONTERO `0.652755/0.001596/0.656491`; in
80--200 Hz they are Riptide `13.494817/0/13.519710` and MONTERO
`1.921207/0.002416/1.957863`. Raw-object/main ratios are approximately
Riptide `1.001737/1.001845` and MONTERO `1.005723/1.019080` for those two
bands; coherent-object/main ratios are Riptide `1.990301/1.871293` and MONTERO
`2.264047/2.148314`. This indicates high internal object coherence, not endpoint
measurement or a bug proof. Object-energy totals remain internal spectral
accounting, not acoustic conservation or a listening-quality claim.

## Short Windows Spatial endpoint diagnostics

Use `scripts\audit-spatial-impulse.ps1 -SelfTest` for the offline contract.
It currently reports `cases=8` and is Windows PowerShell 5.1 compatible.
The script is dry by default; `-Submit -NoBuild -Configuration Debug` is an
explicit live gate that performs three independent 1.5-second impulse probes
(`front`, `left`, and `upper`) with loopback capture. Each report is written
under a unique UTC directory. The companion
`scripts\compare_spatial_impulse_loopback.py` reports onset, peak-normalized
waveform differences, -20/-40/-60 dB threshold tails, left/right energy, and
correlation after aligning each capture at its own first valid transient. It
returns a stop-worthy inconclusive result for silence, partial silence, or
indistinguishable captures. These are loopback diagnostics, not an HRTF,
physical headphone/speaker, or subjective listening oracle.

The 2026-09-01 run is recorded at
`tmp\spatial-live-20260901\spatial-impulse-20260901T034257394Z\`. All three
internal probes passed and the endpoint advertised 128 dynamic objects, but
the selected `Line (3- Steinberg UR12 )` loopback was entirely silent. The
correct conclusion is `INCONCLUSIVE_LOOPBACK_SILENT` /
`STOP_NO_VALID_TRANSIENT`; this is retained as the historical zero-frame
startup case, not as the current impulse procedure.

For a real-media bounded check, the existing
`scripts\audit-atmos-spatial.ps1 -Submit -DisableLfe -MaxAccessUnits 500`
path was run on the provenance-matched Riptide EC-3 sidecar. It completed
Gate6C/Gate7B and endpoint submission and captured 16.23 s of non-silent
Extensible float32 loopback, but remains
`INCONCLUSIVE_ENDPOINT_OUTPUT_NOT_PROVEN`. This verifies the delegate and
capture plumbing only. The report is at
`tmp\spatial-live-20260901\riptide-500au-fixed\Riptide-20260901T034350848Z\`.
The bounded native log is intentionally still QMF-inconclusive because its
500 AUs are below the 1,000-AU coverage gate (`probeExitCode=1`), even though
the wrapper provenance records endpoint submission PASS. Do not promote this
short run to a full-coverage acceptance result.
LFE-off is explicit diagnostic policy (`lfeVolume=0`); no EQ, bed ownership,
or production playback behavior is changed.

The bounded Riptide loopback can be compared with the matching
renderer-neutral Object Direct WAV using
`scripts\compare_spatial_loopback_object_direct.py`. The tool aligns the
first detected active loopback frame, compares the common frame window, and
reports both raw peak/RMS and an RMS-normalized shape correlation. This is a
diagnostic comparison only; endpoint/device startup and processing are
expected variables, and Object Direct is not a binaural or physical-output
reference. In the 500-AU run, 765,073 frames were compared from loopback onset
sample 13,967; correlation was 0.01010 and normalized difference RMS 1.40706.
The v3 band comparison uses per-channel power before averaging (no L/R phase
cancellation) over the analyzed 0--20 kHz bandwidth; its report is
`tmp\spatial-live-20260901\riptide-500au-fixed\Riptide-20260901T034350848Z\loopback-object-direct-comparison-v3.json`.
These numbers do not establish a renderer defect or a listening-quality
result.

If a short impulse capture is dominated by endpoint startup, use the checked-in
diagnostic defaults: `SpatialDynamicProbe --signal impulse --objects 1
--position front --impulse-delay-ms 300` and a 1,500 ms loopback capture,
with an additional 200 ms wait after the capture ready marker. The probe's
delayed impulse is 0.1 (the sine control remains 0.018); the default CLI delay
is 0 for compatibility. In the 2026-09-01 rerun, front/left/upper loopback
responses were position-distinguishable at the normalized waveform layer.
This does not prove HRTF correctness, physical-device output, or listening
quality.
