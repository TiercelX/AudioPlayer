# E-AC-3/JOC/Atmos status

## Status refresh: 2026-09-01 (Gate7C experimental HRTF backend, Riptide bounded A/B)

Gate7C now has an opt-in `--spatial-renderer standard|hrtf` backend and
`--spatial-hrtf-environment small|outdoors` option. `standard` remains the
default path. The HRTF backend uses the Windows SDK
`ISpatialAudioObjectRenderStreamForHrtf` interface with the same Gate6C
15-object PCM/timebase, `StaticObjectTypeMask=None`, exactly 15 dynamic
objects, explicit omni directivity, identity orientation, and explicit Small
or Outdoors environment. `DistanceDecay=nullptr`; it does not call SetGain,
CustomDecay, or alter JOC PCM. It is fail-fast unless LFE is explicitly
disabled (`DISABLED_BY_USER`, volume 0); the HRTF branch does not submit a
static LFE object. Each object buffer is pre-scaled by the evaluated
Gate7B/Spatial property volume (including the program -15 dB headroom), with
finite/overflow checks, before position submission.

Native focused self-test passed (`Eac3SpatialBridgeRendererProbe --self-test`,
27 cases), and PowerShell delegate self-tests passed (play 16, audit 17,
unified runner PASS). A single bounded 500-AU Riptide run was completed for
each environment with unit radius, metadata direction, LFE disabled, Release
no-build, and the same endpoint `Line (3- Steinberg UR12 )`, ID
`{0.0.0.00000000}.{d4305bff-193f-4e1e-9ab2-2bc82c3a6542}`. Input SHA-256 is
`b06c46202e6a36f1bdf867114cb81c3e9e2073cb912036a2bede4d2f3e0fc3ab`.
Both runs had Gate6C/7B/7C PASS, HRTF interface available, endpoint capacity
128, 15 activated objects, 1,600 render commits, 768,000 submitted frames,
11,520,000 pre-scaled samples at volume `0.177828`, and zero LFE samples.
Both loopbacks were non-silent and unclipped. Small channel peak/RMS were
`0.099632/0.014976` and `0.097993/0.014068`; Outdoors were
`0.099632/0.014963` and `0.097993/0.014050`. Analyzer onsets were Small
15,376/15,361 samples and Outdoors 14,896/14,881 samples; these independent
startup offsets are not latency evidence.

Evidence: Small
`tmp/gate7c-hrtf-ab-20260901/small/Riptide-20260901T095338243Z/`, Outdoors
`tmp/gate7c-hrtf-ab-20260901/outdoors/Riptide-20260901T095424653Z/`. The
semantic endpoint-to-endpoint reports are
`tmp/gate7c-hrtf-ab-20260901/comparisons/small-vs-standard-unit.json`,
`outdoors-vs-standard-unit.json`, and `small-vs-outdoors.json`; they use
independent onset alignment, explicit candidate/reference renderer and
environment labels, and per-channel power over the analyzed 0--20 kHz band.
Small-vs-Outdoors normalized correlations were approximately `-0.0177/-0.0205`
with RMS ratios `0.9991/0.9987`; their normalized fractions were respectively
`0.01826/0.01821`, `0.20777/0.20745`, `0.52134/0.52159`, `0.21176/0.21194`,
`0.03378/0.03373`, `0.00709/0.00707` for 0--80, 80--200, 200--375,
375--1000, 1--5k, 5--20k Hz. Small-vs-standard and Outdoors-vs-standard
remain independent endpoint captures with different startup/program state;
their normalized channel correlations were `-0.0363/-0.0542` and
`0.7678/0.7794`, respectively. These are endpoint loopback comparisons only:
the audit result remains
`INCONCLUSIVE_ENDPOINT_OUTPUT_NOT_PROVEN`, and no Dolby equivalence,
production recommendation, or subjective listening claim is made. The
native probe exit code remains 1 because the bounded 500-AU overall probe
also reports its separate below-1000-AU JOC-QMF coverage inconclusive result;
the Gate7C metrics themselves are PASS.

## Status refresh: 2026-09-01 (HRTF capability/impulse A/B)

`SpatialDynamicProbe` now has an endpoint-free-by-default CLI seam for the
official Windows HRTF interfaces: `--renderer standard|hrtf` and
`--hrtf-environment small|outdoors`. Standard remains the default and its
path is unchanged. The HRTF path uses the SDK `SpatialAudioHrtf.h` interfaces,
one dynamic mono float32/48 kHz object, no static LFE, explicit omni-directional
directivity (scaling 0), identity orientation, and an explicit Small or
Outdoors activation environment. DistanceDecay is intentionally `nullptr`
(`activation-null-default`); no JOC/BEAR distance curve is inferred.

One front impulse was run in each environment with 300 ms delay and 1,500 ms
capture duration, using the same endpoint
`Line (3- Steinberg UR12 )`, ID
`{0.0.0.00000000}.{d4305bff-193f-4e1e-9ab2-2bc82c3a6542}`. Both HRTF interface
availability and probe submission passed (`maxDynamicObjects=128`, 150
successful updates, 72,000 submitted frames in Small; 151 successful updates,
72,480 frames in Outdoors). Both loopbacks were non-silent, float32-extensible,
and had zero clip samples. Small peak/RMS were `0.068250/0.003104` at the
capture-report aggregate; Outdoors was the same. Analyzer onsets were about
372.15 ms (Small) and 382.15 ms (Outdoors), with channel peak/RMS
Small L/R `0.068250/0.059020`, `0.0003226/0.0002920`; Outdoors
`0.068250/0.059020`, `0.0002949/0.0002670`. The per-capture-onset comparison
reported normalized correlation `0.999999947` and normalized max difference
`1.36e-5`; LR energy ratio was about `1.220603` and LR correlation about
`0.961939` in both captures. Combined normalized spectral fractions
(`0-80`, `80-200`, `200-1000`, `1-5k`, `5-20k`) were Small
`0.004129, 0.007970, 0.033908, 0.233261, 0.720731` and Outdoors
`0.004173, 0.007929, 0.033919, 0.233262, 0.720717`.

Evidence is under
`tmp\spatial-hrtf-audit-20260901\small\spatial-impulse-20260901T094146952Z\`
and
`tmp\spatial-hrtf-audit-20260901\outdoors\spatial-impulse-20260901T094207834Z\`.
The retained
`tmp\spatial-hrtf-audit-20260901\comparison\small-vs-outdoors.json` used the
position-comparison schema even though both captures were at the same front
position and only the environment changed; it is deprecated and must not be
used as evidence of environment difference or equivalence. The raw capture
statistics above remain capability observations only. These are not Dolby
equivalence, BEAR/JOC integration, physical headphone evidence, or a
subjective listening conclusion. A post-run bookkeeping correction now prints
the activated HRTF object count as 1; the retained first-run logs show the
successful object updates but predate that display-only correction.

## Status refresh: 2026-09-01 (focus=.5 live candidate inconclusive)

A single bounded live candidate was attempted with the matching Riptide EC-3
sidecar (SHA-256
`b06c46202e6a36f1bdf867114cb81c3e9e2073cb912036a2bede4d2f3e0fc3ab`), 500
access units, `DisableLfe`, Release/no-build, unit radius, metadata direction,
and `AzimuthFocus 0.5`. The endpoint was the same `Line (3- Steinberg UR12 )`,
ID `{0.0.0.00000000}.{d4305bff-193f-4e1e-9ab2-2bc82c3a6542}`, but this run
returned `gate7cResult=INCONCLUSIVE` and the delegate exited 1 before writing
spatial provenance. The synchronized loopback capture was also `FAIL` after
2.25 seconds with two transient candidates; it is not a usable A/B waveform.
Evidence is retained under
`tmp\spatial-focus-ab-20260901\focus05\Riptide-20260901T050847036Z\`.
No focus=.5 live position-range or listening conclusion is claimed. The
offline focus 0 versus 0.5 Gate7B evidence below remains the valid candidate
property test; no further endpoint retries were made.

## Status refresh: 2026-09-01 (offline azimuth-focus diagnostic seam)

Gate7B/7C now accept `--spatial-azimuth-focus N` with `0 <= N <= 1`, exposed
as `-AzimuthFocus` by the Spatial PowerShell wrappers and the unified Windows
entry. The default is `0`, preserving metadata direction exactly. For
metadata direction only, the adapter keeps the horizontal radius and vertical
Windows coordinate, computes `theta=atan2(x,-z)`, and uses
`theta'=(1-N)*theta`; the focused position is then
`(h*sin(theta'), y, -h*cos(theta'))`. Thus `N=0` is identity and `N=1`
focuses the horizontal component to front while retaining height and total
radius. Horizontal-zero and fully-zero vectors remain finite and unchanged.
The explicit `PositionDirectionMode front` remains the final fixed-front
override, independent of focus.

Gate7B/7C logs and PowerShell provenance record `azimuthFocus` alongside the
position ranges. PCM, JOC gain/headroom, LFE policy, object count, ramp timing,
and queue behavior are unchanged. This is a candidate non-EQ metadata
focusing diagnostic, not a production-default repair or evidence of improved
listening/HRTF/localization quality. MONTERO offline focus 0 vs 0.5 evidence
is under `tmp\azimuth-focus-offline-20260901\`; no endpoint was started.

## Status refresh: 2026-09-01 (single live unit+front direction A/B)

With explicit authorization for one bounded live run, the matching Riptide
EC-3 input (SHA-256
`b06c46202e6a36f1bdf867114cb81c3e9e2073cb912036a2bede4d2f3e0fc3ab`) was
submitted for 500 access units with `-DisableLfe`, Release/no-build, and
`-PositionRadiusMode unit -PositionDirectionMode front`. The same endpoint
as the clean earlier unit+metadata capture was used:
`Line (3- Steinberg UR12 )`, ID
`{0.0.0.00000000}.{d4305bff-193f-4e1e-9ab2-2bc82c3a6542}`. Gate6C, Gate7B,
and Gate7C passed; Gate7B recorded direction `front`, radius `1.0`,
`X=0,Y=0,Z=-1`, and 7,500 samples. LFE remained
`DISABLED_BY_USER` at volume `0.0`. The wrapper result remains
`INCONCLUSIVE_ENDPOINT_OUTPUT_NOT_PROVEN`; the loopback capture and analyzer
were non-silent and passed their signal-statistics checks.

Evidence is under
`tmp\spatial-direction-ab-20260901\unit-front\Riptide-20260901T045322239Z\`.
The clean unit+metadata reference remains under
`tmp\spatial-radius-ab-20260901\unit\Riptide-20260901T042854793Z\`. Corrected
comparison outputs are
`tmp\spatial-direction-ab-20260901\comparisons\unit-front-vs-reference.json`,
`unit-metadata-vs-reference.json`,
`unit-front-vs-unit-metadata-summary.json`, and `.md`.

Each capture was independently aligned to its own first valid transient:
front onset `13968` samples (`291.000 ms`), metadata onset `13487/13488`
samples (`280.979/281.000 ms`). Over analyzed 0--20 kHz bandwidth, averaged
per-channel normalized fractions were front versus metadata: 0--80 Hz
`0.005680` vs `0.006715`, 80--200 Hz `0.144957` vs `0.162710`, 200--1000 Hz
`0.771971` vs `0.780095`, 1--5 kHz `0.064628` vs `0.036414`, and 5--20 kHz
`0.012764` vs `0.014065`. Front peak max/RMS mean were `0.293073/0.048931`,
metadata `0.310946/0.051395`; L/R RMS ratios were `1.000378` and `1.069330`.
The corresponding per-mode Object Direct comparisons are recorded in the
two `*-vs-reference.json` files and use the same renderer-neutral reference.

This is one endpoint loopback observation of the metadata-vs-fixed-front
diagnostic seam. Independent startup/onset and endpoint state mean spectral,
level, and L/R differences cannot be assigned exclusively to direction. It
is not a listening-quality, HRTF, localization, or headphone-output claim;
no production default was changed.

## Status refresh: 2026-09-01 (live source/unit position-radius A/B)

With explicit user authorization for short playback, the same read-only
`tmp\riptide-spatial-audit-20260901\Riptide.ec3` input (SHA-256
`b06c46202e6a36f1bdf867114cb81c3e9e2073cb912036a2bede4d2f3e0fc3ab`) was
submitted for 500 access units with `-DisableLfe` on the same Release build
and endpoint. The source and unit runs both passed Gate6C, Gate7B, and Gate7C;
the endpoint was `Line (3- Steinberg UR12 )`, ID
`{0.0.0.00000000}.{d4305bff-193f-4e1e-9ab2-2bc82c3a6542}`, with capacity 128
and 15 activated dynamic objects. Gate7B measured source mode radius
`7.071068 -> 7.071068 m`; unit mode measured `7.071068 -> 1.000000 m`,
with 7,500 radius samples in each run. LFE remained disabled (`volume=0`).

The first source capture was subsequently identified as contaminated (18.0 s,
frame-0 signal, peak about `0.84`) and is retained only as superseded evidence;
its `source-vs-unit-summary.json/.md` must not be used. A clean source-repeat
was run once after checking that no capture/probe process remained. Corrected
evidence is under
`tmp\spatial-radius-ab-20260901\source-repeat\Riptide-20260901T043309251Z\`;
the existing unit evidence is under
`tmp\spatial-radius-ab-20260901\unit\Riptide-20260901T042854793Z\`.
The corrected comparator outputs are
`tmp\spatial-radius-ab-20260901\comparisons\source-repeat-vs-reference.json`,
`source-repeat-vs-unit-summary.json`, and `.md`.

Source-repeat and unit now agree within the measured capture tolerance:
unit/source-repeat RMS ratio `0.998736` (`-0.011 dB`), peak ratio `0.999779`
(`-0.002 dB`), and 0--80 Hz fractions `0.008819 -> 0.008632`; 200--375 Hz
fractions `0.540866 -> 0.540902`. Source-repeat onset was sample `13967`
and unit onset sample `13487`; these are independent starts, not a latency
measurement. Both loopbacks were non-silent and unclipped, while the wrapper
remains `INCONCLUSIVE_ENDPOINT_OUTPUT_NOT_PROVEN`. No radius-dependent
spectral/level effect is supported by the corrected pair; no production
default was changed.

## Status refresh: 2026-09-01 (diagnostic position-radius A/B)

Gate7B/7C now accept `--spatial-position-radius source|unit`, exposed by
`scripts\play-atmos-spatial.ps1 -PositionRadiusMode`. `source` is the default
and preserves the existing listener-relative metre coordinates. `unit`
preserves each nonzero position's direction while normalizing its radius to
1 metre; the zero vector remains a finite zero vector and is not assigned an
arbitrary direction. Gate7B reports the selected mode plus source/output
radius min/max/mean and sample count, so the A/B is auditable. Gain, headroom,
LFE policy, object count, ramps, and queue behavior are unchanged.

The same option is forwarded by `scripts\audit-atmos-spatial.ps1` for its
delegate contract. Focused adapter, renderer-option, and PowerShell tests
cover default compatibility, direction preservation, unit length, zero-vector
handling, and propagation. This is a diagnostic endpoint-submission A/B only;
it is not a production-default change, a BEAR change, or evidence of
localization/listening quality. No live endpoint was started for this update.

## Status refresh: 2026-09-01 (diagnostic position-direction A/B)

Gate7B/7C now accept `--spatial-position-direction metadata|front`, exposed
by `scripts\play-atmos-spatial.ps1 -PositionDirectionMode` and forwarded by
`scripts\audit-atmos-spatial.ps1`. `metadata` is the default and preserves
the existing room-to-Windows direction. `front` preserves the selected
`source` or `unit` radius for every nonzero position and sets the Windows
listener-relative direction to `(0,0,-radius)`; zero vectors remain zero.
The direction mode is recorded in Gate7B/7C text output and PowerShell
provenance. PCM, JOC gain/headroom, LFE policy, object count, ramp timing,
and queue behavior are unchanged. Adapter and renderer self-tests cover
default compatibility, front-axis conversion, `unit+front`, zero vectors,
invalid values, and a ramp whose evaluated midpoint remains on the front
axis. This is an offline diagnostic seam for metadata-vs-fixed-front A/B, not
a production default change or evidence of HRTF/localization/listening
quality. No endpoint was started for this update.

## Status refresh: 2026-09-01 (Spatial LFE disable A/B seam)

Gate7C now accepts the explicit native flag `--spatial-disable-lfe`, exposed
by `scripts\play-atmos-spatial.ps1 -DisableLfe` and the unified Windows
entry's `-Mode Spatial -DisableLfe`. The static LFE object remains activated,
buffered, finite-counted, and included in exact buffer accounting, but its
single successful `SetVolume()` call is `0.0` and the report records
`lfePolicy=DISABLED_BY_USER`. The default path is unchanged:
`lfePolicy=PROGRAM_HEADROOM`, LFE volume `0.177827941` (`-15 dB`). Dynamic
objects, their `-15 dB` applied gain (`dynamicGainHeadroomDb=15` in the report),
positions, ramps, and queue settings are not changed. The switch is rejected
outside Spatial mode / without Gate7C rather than silently affecting BEAR or
other probes.

The focused self-tests cover default/disabled native LFE policy and delegate
argument propagation. Bounded `media\MONTERO.ec3` A/B runs at
`-MaxAccessUnits 8` passed Gate6C/7B/7C for both policies; reports are in
`tmp\lfe-ab-default4\spatial-provenance.json` and
`tmp\lfe-ab-disabled9\spatial-provenance.json`. Both retained 15 dynamic
objects, and the disabled run recorded `lfeVolume=0.0`; these are endpoint
submission results only (`probeExitCode=1` on the short run), not loopback or
audible-improvement evidence. This control does not bypass endpoint-side bass
management.

## Status refresh: 2026-09-01 (unified Windows entry)

Added scripts/run-atmos-windows.ps1 as a safe Windows entry point. Its
default Validate mode creates a timestamped report directory and performs
input-extension, BEAR data/hash, native probe, and ffprobe dependency checks;
it does not submit audio. Empty InputPath opens a Windows Forms picker
restricted to .m4a/.mp4/.mka/.mkv/.eb3/.ec3/.eac3. ADM/WAV input is
explicitly rejected.

Mode Render -BearProfile OfficialMain delegates to the existing offline BEAR
exporter with pinned main commit 6127e897b941211051c2ad135ee09b00be2e6ae0
and default_v1.1.tf SHA-256
171acae2159e60ffe9d705abc16a79be129ecd06d37186fae413a265b6ed71e8.
Mode Render -BearProfile SystemHV6 is an explicit v6 experiment using
system-h-22-v6.tf SHA-256
8195b0b456f9172a709375f9da8e2a39c63d9d5f7b03aa02ee90359a8bffe7c9, the
22-channel 9+10+3 layout, and missing B+135/B-135. It does not change the
default 24-channel data or production playback.

Mode Spatial delegates play-atmos-spatial.ps1: real
ISpatialAudioObjectRenderStream endpoint submission of Gate 7C objects,
non-BEAR, no audio-file output. The unified Validate mode intentionally uses
dry dependency checks instead of running an audio-producing spatial probe.
Both thin .cmd launchers prefer `pwsh.exe` when available, otherwise use
`powershell.exe`; script paths are quoted and additional named arguments pass
through. These helpers do not establish
subjective listening, synchronized loopback, Dolby equivalence, or production
dynamic-object playback.

The same-day endpoint probe evidence is bounded and separate from the helper
dependency check: SpatialDynamicProbe reported
spatialStreamAvailable=1 but maxDynamicObjects=0, then returned INCONCLUSIVE
before submitting any dynamic audio. SpatialBedProbe printed its internal
PCM-submission PASS line, but the process exited with 0xC0000374 during
cleanup; this is recorded as FAIL_CLEANUP_CRASH and is not an endpoint PASS.
Neither result changes the independent validate-all PASS.

The bounded unified SystemHV6 render was also attempted with
media\MONTERO.eb3 and 157 access units in
tmp\windows-atmos-system-h-v6-render-final. Profile source HEAD and TensorFile
existence/SHA passed; the then-configured fixed py312 interpreter preflight
also passed, but that runtime choice is now retained only as historical
evidence and is not used by the unified delegate. The
delegated native Gate6C step stopped before BEAR Python import with
packet1:unexpected-bytes-at-6656. The complete log is
tmp\windows-atmos-system-h-v6-render-final\MONTERO-render-20260831T180848749Z\MONTERO-native-bundle.txt.
No BEAR output or TensorFile overwrite occurred; this is an input/container
boundary, not evidence that the SystemHV6 renderer passed. The wrapper was
not relaxed to bypass this failure.

The follow-up packet-copy audit is in
tmp\eb3-normalization-audit\packet-compare.json. The bundled ffmpeg
(build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc\bin\ffmpeg.exe) does
not provide an eac3 muxer. The available system ffmpeg command
-map 0:a:0 -c copy -f eac3 succeeded, but its output was byte-identical to
MONTERO.eb3 (SHA-256
aee925b02628b41128df08a2eb683a6ce6799a59cb9f703af5e8b312f320365d,
35,434,992 bytes, 5,312 packets, 170.360533 seconds). Its first packet is
16 bytes and subsequent packets are 6,672 bytes, so this is not removal of
the observed 16-byte EB3 wrapper; the native probe rejects both files at
packet 1 offset 6,656. media\MONTERO.ec3 is a different 6-channel,
5,311-packet, 169.952000-second stream (SHA-256
c91a662edd693d8a909e1780d9dea2831624fbf19ba3b8ed5e5a9c6fce81ff9e).

The unified SystemHV6 retry with media\MONTERO.ec3 reached native Gate6C
and produced 158 bundle batches, but the then-configured fixed py312
delegation stopped at ModuleNotFoundError: No module named visr_bear.api.
This historical run confirms a binding/runtime issue after native assembly;
no output was accepted as a BEAR render. Evidence is under
tmp\windows-atmos-system-h-v6-render-ec3\MONTERO-render-20260831T181259206Z\.
No automatic EB3 normalization was added because packet-copy did not change
the bytes and the format-specific unwrapping rule is not yet validated.

The corrected unified delegate no longer passes a duplicate `BearRoot` or
`BearPython`; this lets `export-atmos-binaural.ps1` select the verified main
`build-visr-bear-6\python\Release` import path. A bounded native EC3 rerun then
completed BEAR rendering with `media\MONTERO.ec3`, `-MaxAccessUnits 157`, and
`-NoBuild`: 157 access units, 158 bundle batches, 241,152 output frames,
5.024 seconds, finite stereo float32 output. Evidence is under
`tmp\windows-atmos-system-h-v6-render-ec3-default-import\MONTERO-render-20260831T182004502Z\`;
the raw output SHA-256 is
`c92f21e41231c109eed161f75b4b56abb4e1d4fa49be3cc46dc9b9dd64360da1`.
The export provenance records main commit `6127e897` and v6 data SHA
`8195b0b456f9172a709375f9da8e2a39c63d9d5f7b03aa02ee90359a8bffe7c9`, and the
actual default binding runtime is the existing `tmp\reference\ear-2.1.0`
Python environment. The earlier explicit py312 attempt remains a historical
`visr_bear.api` import failure; it is not overwritten by this successful
default-import-path result.

The full-file EB3 audit is
`tmp\eb3-structure-audit\montero-eb3-structure.json`: all 5,311 AUs pass
the observed `[16-byte wrapper][2,560-byte legacy AC-3][4,096-byte dependent
E-AC-3]` pattern, with 10,622 syncframes and 6,656-byte payloads. Wrapper
prefixes are `01100000` for AUs 0--82 and `01100001` for AUs 83--5310;
all wrappers end in `00088000`. The carriage bytes contain invalid BCD
nibbles (1,955/42,488), and their decoded 32-bit values repeat and move
backward, so no continuous BCD/timecode interpretation or SMPTE ST 339
identification is established. The stripped payload SHA is
`027533cccb3fc02aaad39673b54036a09bd9fdf304084be26ecad7483510abcc`, not
the different 16,315,392-byte `MONTERO.ec3` stream. The existing native probe
still rejects this MONTERO wrapper variant at offset 6,672; therefore no
automatic unwrapper is proposed and no EB3 render PASS is claimed.

## Status refresh: 2026-09-01 (Windows launcher hash compatibility)

The drag-and-drop `render-atmos-windows.cmd` path was compatible with the
render arguments but failed in one user's PowerShell environment before
rendering because `Get-FileHash` was unavailable. `Get-FileSha256` in the
unified entry, BEAR export delegate, and Spatial delegate now uses only
`.NET` `SHA256.Create()` over a disposed `FileStream`, returning lower-case
hex without depending on the PowerShell utility cmdlet. The two thin `.cmd`
launchers now prefer the first `pwsh.exe` found by `where`, falling back to
`powershell.exe`; the remaining UTF-8 writer was also changed from
`[Type]::new` to `New-Object` for Windows PowerShell compatibility.

Evidence: all three PowerShell 5.1 and PowerShell 7 self-tests pass, including
the fixed `abc` SHA-256 vector
`ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad`.
The Spatial delegate report writer also uses .NET UTF-8 without
`utf8NoBOM`, so a direct Windows PowerShell 5.1 bounded submission completes.
The user-shaped launcher invocation with `media\MONTERO.ec3`,
`-MaxAccessUnits 8`, and `-NoBuild` completed `exportAtmosBinaural=PASS`;
the separate `-SelfTest -OpenOutput` invocation also passed parameter
binding without opening Explorer. `scripts\validate-all.ps1 -NoBuild`
remains PASS for unit tests, report schema, and smoke. This fixes a launcher
compatibility boundary only; it does not change the BEAR data, renderer, or
endpoint claims.

## Status refresh: 2026-09-01 (BEAR main System H v6 cross-seed stability)

The independent v6 experiment retained official BEAR main commit
`6127e897b941211051c2ad135ee09b00be2e6ae0` and official `default_v1.1.tf`
SHA-256 `171acae2159e60ffe9d705abc16a79be129ecd06d37186fae413a265b6ed71e8`.
It used the explicit measured `9+10+3` layout, official `--default-schedule`,
90,000 view steps and 840,000 global steps, with independent seeds view
`16127` and global `16128`; v5 used seeds `6127`/`6128` at the same budgets.
The BBC System H input remains 22 emitters and omits `B+135`/`B-135` from
the default 24-channel layout. No missing IR was guessed or synthesized, and
the experiment does not change the default 24-channel data or export path.

The v5-to-v6 numerical stability gates passed: view delays max absolute
difference `1.043938` samples, RMS `0.198061`, minimum correlation
`0.999989905`; global delays max absolute `0.182405`, RMS `0.0200646`,
correlation `0.999997663`. The final BRIR comparison was max absolute
`0.226055`, RMS `0.000591404`, correlation `0.9989191625`. The declared
gates were view `<=1.5` samples / `<=0.25` RMS, global `<=0.5` / `<=0.05`,
and MONTERO per-ear correlation `>=0.99998` with difference RMS `<=0.0006`.

The v6 TensorFile is
`tmp/reference/bear-main-6127e897/ir-processing-system-h/v6/layout-9+10+3/system-h-22-v6.tf`,
SHA-256 `8195b0b456f9172a709375f9da8e2a39c63d9d5f7b03aa02ee90359a8bffe7c9`;
it is `[180,22,2,2976]`, contains the official HOA decoder, and all BRIR/HOA
values are finite. The seven-direction BEAR binding impulse probe
(front/left/right/rear/upper/lower/interior) passed. The same 4,799,423-frame
MONTERO bundle gave per-ear correlations `[0.9999895132, 0.9999880578]`,
difference RMS `0.000451254`, and 10-second tail-energy ratio `0.999987829`.
Repeating the render produced identical raw-f32 SHA-256
`bc1ba6541a2fcd156ee9eb3dae078623b99c85a234fdb918fa037d5eda0f5a9b` and
safe -2 dB S24 SHA-256
`3d296fc8c5dda7f00e10080c0ec21a9adc23eca49e65f26448d531a5c1d69802`.

This is `SOLUTION_STABLE_WITH_LATE_STOCHASTIC_IMPROVEMENTS`: the independent
seed outputs are numerically stable under the declared gates, but the logs
still show late stochastic improvements (view last improvement
`81000--89100/90000`; global `554400/840000`). It is not optimizer-internal
plateau convergence, formal calibration, subjective listening evidence,
endpoint evidence, or Dolby equivalence. `scripts\\validate-all.ps1` passed
unit tests (10 suites), report schema, and smoke; the v6 evidence is under
`tmp/reference/bear-main-6127e897/ir-processing-system-h/v6/`. v2--v5 remain
retained as historical feasibility/non-convergence experiments.

## Status refresh: 2026-08-31 (BEAR main alignment)

The offline BEAR entry points now default to the official BEAR `main` commit
`6127e897b941211051c2ad135ee09b00be2e6ae0` and data file `default_v1.1.tf`
(SHA-256 `171acae2159e60ffe9d705abc16a79be129ecd06d37186fae413a265b6ed71e8`).
The new checkout, build, and data remain isolated under
`tmp/reference/bear-main-6127e897`; the old `tmp/reference/bear-git` and
`docs/dev/reference-cache/bear-default.tf` are retained for explicit A/B.
`scripts/export-atmos-binaural.ps1` reports source commit/version and data
version/hash in its export provenance. `run_bear_montero_bundle.py` records the
same fields, and `run_single_object_oracle.py` now uses the new defaults.
Explicit `-BearRoot`/`-BearData` still select the legacy reference.

The new Windows x64 binding built successfully against the existing VISR 0.13.0
install and Boost 1.85; configure/build evidence is under
`tmp/reference/bear-main-6127e897/bear-main-configure-6.log` and
`bear-main-build-final.log`. The front-centre impulse smoke is `PASS` in
`bear-main-front-center-impulse-final.json`. The initial CTest invocation lacked
the VISR runtime DLL directories in `PATH`, causing `test_renderer.exe` to exit
with Windows loader status `0xC0000135`; this was a harness-environment issue,
not a renderer assertion failure. With the same explicit VISR/BEAR DLL paths as
the render wrapper, all six BEAR tests pass in 11.60 s, including all four
`test_renderer` cases in 2.21 s.
The CMake install target's three generated host-site package items were moved
to ignored `tmp/reference/bear-main-6127e897/site-packages-install/`; probes
use explicit build/VISR paths and do not depend on that global location.
The user-facing wrapper smoke also passed for 157 AUs / 241,152 frames / 5.024 s
under `tmp/reference/bear-main-6127e897/export-wrapper-smoke2/`, with
provenance identifying `main@6127e897` and `default_v1.1`. Its external-command
capture tolerates native stderr diagnostics under Windows PowerShell while
retaining exit-code failure handling.

The representative same-bundle MONTERO render processed 4,799,423 frames
(99.987979 s), accepted 46,875 metadata updates, and produced finite 48 kHz
stereo float32 plus -2 dB S24 outputs under
`tmp/reference/bear-main-6127e897/montero-100s-output-v2/`. For a controlled
numeric A/B, old pyd/old data and new main/new data rendered the identical
bundle; `ab-old-build-vs-new-data.json` reports difference RMS `9.46e-9` and
per-channel correlation above `0.99999999999998`. This establishes numeric
compatibility for this object-only bundle, not audible quality, endpoint
output, or Dolby JOC equivalence. The new main branch's added IR-processing
toolchain is present in source but was not used to regenerate the existing
TensorFile.

## Status refresh: 2026-08-31 (BEAR main IR-processing / BBC System H)

The official `bear/process_irs` pipeline was exercised against the existing
BBC System H SOFA at `docs/dev/reference-cache/renderer-assets/bbcrdlr_systemH.sofa`
(SHA-256 `09dc3414a5eb7d9a325e0ad750da87ce63c9d4baf270c980296c726e152c89fa`).
The input is `MultiSpeakerBRIR`, 48 kHz, 180 views, 22 emitters, 2 receivers,
zero `Data.Delay`, and finite `Data.IR` samples. BEAR's default
`9+10+3_extra_rear` layout has 24 channels; the only missing channels are
`B+135` and `B-135`. With either official `smart` or `closest` selection, the
24-channel extraction explicitly fails with `IR selection returned duplicate
IRs`; no missing IR was synthesized, copied, or interpolated.

An explicit 22-channel `9+10+3` extraction succeeded, followed by official
delay-over-view and global-delay stages at the recommended `--default-schedule
--opt-steps 10000`, then finalise and report. The v2 1000-step output remains
under the same root as a feasibility artifact. The v3 TensorFile is
`tmp/reference/bear-main-6127e897/ir-processing-system-h/v3/layout-9+10+3/system-h-22-v3.tf`
(SHA-256 `3f4e18d14b32d95a2d1b43a4820b25d7d5cdfc9cd6af5873021607c16d2dd47d`),
with 180 views and `[180,22,2,2976]` finite BRIRs, gain normalisation, and HOA
decoder data. However, the v3 PDF's global last improvement is at about
7800/10000, while the 44 view/speaker/ear logs improve as late as 10000, so
this remains `INCONCLUSIVE_OPTIMISATION_NOT_CONVERGED`, not a calibrated PASS.
IR/window/positions/ITD plots are structurally sane. A separate BEAR binding
impulse probe still passes finite front/left/right/rear/upper checks, but does
not override the convergence result. This 22-channel data file is not a
drop-in replacement for the default 24-channel data and was not made a new
export default.

The identical 4,799,423-frame / 99.987979-second MONTERO bundle rendered with
the v3 System H TensorFile produced finite 48 kHz stereo output and a safe -2
dB S24 audition copy. Numeric A/B against `default_v1.1` gave correlation
`[0.9971962, 0.9968446]`, difference RMS `0.00735452`, and 10-second tail
energy ratio `0.99992`; against the old raw System H oracle, correlation was
`[-0.00342, -0.00181]`, demonstrating a different chain. These are numerical
comparisons only, not listening or Dolby-equivalence claims. The processing
environment is isolated under `tmp/reference/bear-main-6127e897/ir-processing-system-h/`;
Python 3.12 was used because the pinned pipeline's ECOS dependency has no
Python 3.14 wheel; the isolated package record is
`py312-venv-pip-freeze.txt` (SHA-256
`ea1ff66fba07294af630caa479057768c25267f534f0ed82b95faab4f8bbed4`). Existing default data, SOFA, old outputs, and user files
were not overwritten. The focused bundle-wrapper unit test passed 6/6 and
`scripts\validate-all.ps1 -BuildDir build-mm -Configuration Debug` passed its
unit-test and report-schema stages but failed the pre-existing WASAPI smoke
gate (`positionStallOrLagDetected=True`, `harnessResult=FAIL`); this is retained
as `validate-all-system-h.log` and is unrelated to the offline BEAR data.

## Status refresh: 2026-08-31 (EAR System H + BBC BRIR oracle)

The bounded alternative renderer now has a small offline-only native stage:
`Eac3SystemHBrirOffline` consumes validated interleaved 48 kHz 22-channel
float32 System H PCM, applies the checked-in 22-emitter BBC BRIR cache, flushes
the 16,383-sample tail, rejects non-finite input, and refuses partial/invalid
frame input. Its focused self-test passed 4/4, including cache emitter order,
tail length, truncation, and non-finite input. This target is not used by the
player or decoder.

Pinned EBU EAR 2.1.0 was verified against `media/MONTERO.wav`: its official
`Renderer(bs2051.get_layout("9+10+3"))` produced 10 DirectSpeakers + 56
Objects + 0 HOA into 24 channels (22 main + 2 LFE). The wrapper keeps the LFE
channels as a separate sidecar and sends only the 22 non-LFE buses to the
project BRIR. The Logic stream graph needed 66 in-memory pack-reference
repairs; the selected source LFE track is mapped from its room-centric label to
`LFE1`, while the second sidecar slot (`LFE2`) is silent for this single-LFE
source. EAR's documented timing fixes are also applied. The ADM source was not
rewritten.

Evidence: `tmp/montero-system-h-oracle-r1/` contains the 0-100 s EAR bus,
LFE sidecar, native stereo f32 output (4,816,383 frames including the BRIR
tail), and a safe S24 audition WAV. A read-only comparison against the Logic
Dolby bounce in `comparison2/` found 90-100 s correlation of only
`[-0.0192, -0.0725]`, residual `0.9989`, and spectral cosine `0.7331` after
automatic alignment; the candidate raw peak was `+2.521 dBFS`. The wrapper's
deterministic safe-audition gain is `-3.5209 dB` (requested -2 dB retained as
a floor), leaving 1 dB headroom and no S24 clipping. This is
renderer-difference evidence, not a Dolby-equivalence claim. EAR emitted many
timing-repair warnings; the exact command and logs are retained in the
directory. The wrapper smoke (`tmp/montero-system-h-oracle-wrapper-smoke2/`)
passed end-to-end for 0.1 s using the complete local FFmpeg for WAV export.

## Status refresh: 2026-08-31 (Spatial endpoint gate repair)

The first MONTERO spatial attempt reached the Windows endpoint capability and
activation path (`maximumDynamicObjects=128`, reported capacity `15`), but
all 4306 access units were rejected upstream by the additional-OAMD policy:
`reserved-trim-warp-mode` produced zero Gate6C batches, followed by Gate7B
failure and Gate7C `prebufferEmpty`/`E_FAIL`. This was not evidence of an
endpoint-capacity or activation failure. `access-unit.cpp` now applies the
same opt-in additional-element policy to `jocGate7c` as to BEAR export and
matrix tracing; the unsupported additional element is excluded from the
Gate6C metadata state while the core B2B state remains audited.

The bounded real endpoint check then passed with the existing MONTERO raw
E-AC-3 sidecar, without re-encoding:

    .\scripts\play-atmos-spatial.ps1 -InputPath tmp\montero-spatial\01.-MONTERO-Call-Me-By-Your-Name-native-copy.ec3 -OutputDir tmp\montero-spatial-repro-157-fixed -MaxAccessUnits 157 -NoBuild

Evidence is under `tmp/montero-spatial-repro-157-fixed/`. It contains 157
access units, 158 Gate6C batches, 2355 committed Gate7B updates, 15 activated
dynamic objects, 4 prebuffer batches, 158 pushes/pops, zero producer
timeouts, zero underruns, and `gate6cResult=PASS`, `gate7bResult=PASS`,
`gate7cResult=PASS`. The probe process exit code is `1` only because its
short-run Gate5C coverage is `INCONCLUSIVE` below 1000 AUs; the wrapper now
reports the three layer results separately and accepts this diagnostic exit
code when all spatial submission layers pass. This remains endpoint
submission evidence only: it does not prove audible device output or
loopback equivalence.

## Status refresh: 2026-08-31 (user-facing offline BEAR and Spatial entry points)

The checked-in `scripts/export-atmos-binaural.ps1` wrapper now provides the
shortest official-BEAR path for an E-AC-3/JOC file: it selects one E-AC-3
stream from `.m4a/.mp4/.mka/.mkv` (or accepts raw `.eb3/.ec3/.eac3`), packet-
copies container input to an ignored `.ec3` sidecar, runs
`Eac3AccessUnitProbe --joc-gate6c --joc-bear-export`, and invokes the pinned
Python BEAR bundle consumer in explicit `--full-file` mode. The default is
full-file; `-MaxAccessUnits N` is an explicit bounded-test option. Output is
48 kHz stereo `pcm_f32le` WAV plus a fixed -2 dB audition copy and
`export-provenance.json`; no normalization is applied to the raw output. The
full-file writer is streaming (it does not retain the complete stereo result
in RAM), discards the measured 167-sample renderer latency, flushes the
delayed tail, and writes exactly one output frame per input bundle frame.
The provenance records `algorithmicLatencySamples=167`, input/output frame
counts, and that LFE is parsed for bundle validation but excluded from stereo
BEAR mixing. Unless `-NoBuild` is supplied, the wrapper builds the selected
configuration's `Eac3AccessUnitProbe` target before resolving the executable.
It verifies both raw float32 and -2 dB `pcm_s24le` audition outputs with
matching frame counts; the latter is printed as
`recommendedListeningOutput` for normal listening.

The bounded smoke used an existing r4 EC3 without DEE re-encoding:

    .\scripts\export-atmos-binaural.ps1 -InputPath tmp\oracle\single-object-oracle-bounded-5s-r4\ec3\single-object-01.ec3 -OutputDir tmp\oracle\atmos-binaural-r9-final -MaxAccessUnits 157 -NoBuild

It passed with 157 access units, 158 bundle batches, 241152 input and output
frames, and 5.024 seconds. Evidence is under
`tmp/oracle/atmos-binaural-r9-final/`; the native diagnostic log retains
`bearExport=PASS` and the expected below-1000-AU Gate5C `INCONCLUSIVE`.
The legacy `run_bear_montero_bundle.py` fixed-window mode remains compatible;
its new `--full-file` mode is the generic path used by the wrapper.

The companion `scripts/play-atmos-spatial.ps1` prepares the same raw/container
inputs and calls `Eac3AccessUnitProbe --joc-gate7c`, defaulting to the complete
file and allowing explicit `-MaxAccessUnits` for tests. It submits 15 dynamic
objects plus static LFE to the default Windows Spatial Audio endpoint and
writes only logs/provenance; it does not export a file. Its self-test passes
4/4. This remains endpoint-submission evidence requiring manual listening or
loopback for output proof. The production WASAPI route still submits the
existing eight-channel static 5.1.2 bed, not the diagnostic 15-object JOC
route. No MONTERO full-file endpoint run was performed in this slice.

The public EBU ADM remains a framing fixture rather than a drop-in DEE source:
the original fails `ATMOS_STORAGE_RES_FORMAT_INVALID`, and an EAR-completed
derivative reaches `Content was not authored with Dolby tools`.

The first bounded user-listening artifact was generated from the existing
MONTERO container without transcoding:

    .\scripts\export-atmos-binaural.ps1 -InputPath media\01. MONTERO (Call Me By Your Name).m4a -OutputDir tmp\oracle\atmos-binaural-montero-1875-final -MaxAccessUnits 1875 -NoBuild

Wall time was 25.275 seconds. The run completed with native exit 0,
1875 access units, 1876 bundle batches, and 2,880,000 input/output frames
(60.000 seconds; the source container is 137.768 seconds and was deliberately
bounded). Raw output peak/RMS was 0.8327285/0.0609322 with clipping false;
the recommended -2 dB `pcm_s24le` audition output was
`tmp/oracle/atmos-binaural-montero-1875-final/01.-MONTERO-Call-Me-By-Your-Name-full-file-audition-minus2dB-s24.wav`,
peak/RMS 0.6614597/0.0484001, clipping false. Raw and audition SHA256 are
`120fdff14e967e643684df84dabd093beb3942babbfadf63659e211671237a4f` and
`43b14baf9ad84b30a7b27e651c39f75b7d66af0da02b972f80cbbcc6ee36e046`.
Gate6C math/reconstruction, metadata order, and continuity counters were zero;
the log still records the known additional-OAMD unsupported-element count
(`oamdB2bAdditionalElementFailureCount=1875`). This is a listening artifact,
not a Dolby-equivalence claim.

### 512-sample phase correction after user jitter report

The original `montero-1875-final` artifact used an incorrect streaming loop:
it restarted 512-sample BEAR processing at each BSCN batch. Because the first
batch is 959 samples while the normal batches are 1536, this permanently
shifted the renderer block phase by 447 samples. A 5-second identical-bundle
A/B reproduced the issue: the corrected output equals the prior concatenate
reference after the expected 167-sample latency shift (`correlation=1.0`,
`difference RMS=0`). The fix is a bounded carry buffer that preserves global
512-sample alignment across transport batches.

The corrected MONTERO artifact is under
`tmp/oracle/atmos-binaural-montero-1875-phase-fixed/`, with the recommended
file
`01.-MONTERO-Call-Me-By-Your-Name-full-file-audition-minus2dB-s24.wav`.
For the corrected run, bundle-boundary jump p99/max was `0.05307/0.16266`
versus `0.04381/0.22828` for the phase-reset artifact; normalized to each
run's all-sample distribution, the corrected boundary p99 is `1.00x` versus
`1.06x`, so bundle boundaries are no longer a special discontinuity source.
The corrected raw float output peaks at `1.07701` (no hidden normalization),
while the recommended -2 dB audition output peaks at `0.85550` and does not
clip. Gate6C math/reconstruction/order/continuity counters remain zero;
additional OAMD unsupported-element reporting remains the known separate
limitation.

## Status refresh: 2026-08-31 (payload-backed forensic audit)

The forensic summary now independently decodes each AU's lossless `payloadHex`
and compares every recorded codeword to the payload bits at its offset/length.
It also requires contiguous object headers, contiguous object data after the
final header, contiguous DP symbol data, exact symbol adjacency, expected
dense/sparse symbol ordering, and independent raw-symbol-to-q reconstruction.
Absent objects use zero bands/data points and empty q/data ranges. Older traces
without forensic records remain compatible; the earlier
`tmp/oracle/joc-matrix-trace-forensic-r1/` lacks payload bytes and is therefore
historical only.

The final payload-backed evidence is
`tmp/oracle/joc-matrix-trace-forensic-r2/`. Both cases contain 157 AUs,
141,300 payload-verified symbols, 141,300 q cells, 2,355 contiguous object
header/data ranges, and 2,355 contiguous symbol-data ranges. Both report
`qMismatchCount=0`, `mismatchCount=0`, `maxAbs=0`, and
`maxRelativeRms=0`; both are dense fixtures, so sparse symbol behavior remains
synthetic self-test coverage. The summaries were generated with:

    build-mm\Release\Eac3AccessUnitProbe.exe tmp\oracle\single-object-oracle-bounded-5s-r4\ec3\single-object-01.ec3 --max-units 157 --oamd --joc --pcm --joc-gate6c --joc-table docs/dev/ts_103420_tables.c --joc-matrix-trace tmp\oracle\joc-matrix-trace-forensic-r2\single-object-01.jsonl --summary
    build-mm\Release\Eac3AccessUnitProbe.exe tmp\oracle\single-object-oracle-bounded-5s-r4\ec3\single-object-02.ec3 --max-units 157 --oamd --joc --pcm --joc-gate6c --joc-table docs/dev/ts_103420_tables.c --joc-matrix-trace tmp\oracle\joc-matrix-trace-forensic-r2\single-object-02.jsonl --summary

    py -3 scripts\summarize_joc_matrix_trace.py --input tmp\oracle\joc-matrix-trace-forensic-r2\single-object-01.jsonl --output tmp\oracle\joc-matrix-trace-forensic-r2\single-object-01.summary.json
    py -3 scripts\summarize_joc_matrix_trace.py --input tmp\oracle\joc-matrix-trace-forensic-r2\single-object-02.jsonl --output tmp\oracle\joc-matrix-trace-forensic-r2\single-object-02.summary.json

The probe logs retain the expected Gate5C below-1000-AU `INCONCLUSIVE` status;
Gate6C and matrix trace execution passed. Final project validation was:

    scripts\validate-all.ps1 -BuildDir build-mm -Configuration Debug

It reported 3/3 PASS (unit tests, report schema, and smoke test).

The dense L/R public normative chain has no local parser/renderer mismatch at
this layer. Stop evidence-free parser or renderer changes; the next priority
is an external Dolby decoder, an independent JOC implementation, or a same-
source encoder A/B to distinguish DEE decomposition from Dolby equivalence.
Sparse and `present=0` behavior remains a separate low-priority real-fixture
gap.

## Status refresh: 2026-08-31 (opt-in Gate5A forensic cursor trace)

The matrix-trace path now requests an opt-in Gate5A forensic capture; ordinary
JOC parsing keeps the capture disabled. Each trace AU records payload bit
count and syntax end, explicit object/data-point ranges, and unabridged symbol
bit strings classified as `sparse-fixed-channel`,
`sparse-huffman-channel-delta`, or `huffman-coefficient`. The existing `q`
arrays remain differential-quantized indices, while `dq` and representative
interpolation remain Gate5B outputs. A synthetic Gate5A vector covers cursor
ranges, the fixed 3-bit channel, Huffman symbols, dense channel/band mapping,
sparse modulo mapping, and truncated-payload rejection.

The two-pass syntax is represented explicitly: object `headerBitOffset` /
`headerBitEnd` is separate from its optional `dataBitOffset` /
`dataBitEnd`; each data point separately reports optional `offsetTs` range and
`symbolData` range. Symbols carry `parameterBand`, dense `inputChannel`, and
sparse `resolvedInputChannel` (or JSON `null` when not applicable), so a raw
symbol can be joined to the stored data-point/channel/band and subsequent
differential-q cell without treating a wide noncontiguous envelope as one
range.

A one-AU schema smoke passed with the corrected Release probe:

    build-mm\Release\Eac3AccessUnitProbe.exe tmp\oracle\single-object-oracle-bounded-5s-r4\ec3\single-object-01.ec3 --max-units 1 --oamd --joc --pcm --joc-gate6c --joc-table docs/dev/ts_103420_tables.c --joc-matrix-trace tmp\oracle\joc-forensic-smoke\single-object-01-contract-v3.jsonl --summary

The log is `tmp/oracle/joc-forensic-smoke/single-object-01-contract-v3.log` and the
JSONL is local ignored evidence. Gate5A, Gate5B, matrix self-test, Gate6C,
and the forensic schema all passed; the process status is 1 only because the
existing Gate5C smoke coverage is below its 1000-AU threshold. This was only
the intermediate one-AU schema stage and was superseded by the payload-backed
r2 audit above. No renderer/BEAR or
production DSP math was changed. The forensic self-test also proves raw sparse/dense symbols join to
their data-point parameter-band/channel cells; Gate5B's existing differential
recurrence test remains the separate q-cell arithmetic check.

The earlier full bounded audit under
`tmp/oracle/joc-matrix-trace-forensic-r1/` is retained as historical evidence;
it predates payloadHex and must not be treated as independently codeword-
verified. The payload-backed r2 audit above supersedes it.

The historical r1 summaries were produced with:

    py -3 scripts\summarize_joc_matrix_trace.py --input tmp\oracle\joc-matrix-trace-forensic-r1\single-object-01.jsonl --output tmp\oracle\joc-matrix-trace-forensic-r1\single-object-01.summary.json
    py -3 scripts\summarize_joc_matrix_trace.py --input tmp\oracle\joc-matrix-trace-forensic-r1\single-object-02.jsonl --output tmp\oracle\joc-matrix-trace-forensic-r1\single-object-02.summary.json

The validator is fail-closed for malformed forensic records and q mismatches;
the compact summaries retain counts and hashes while the large JSONL files
remain local diagnostic evidence.

## Status refresh: 2026-08-31 (bounded config3 normative matrix trace)

`Eac3AccessUnitProbe` now has the opt-in `--joc-matrix-trace <path>` diagnostic
path. It captures the config3 input identity order (`FL,FR,FC,SL,SR`), per-AU
QMF input energy, object syntax/quantized and dequantized coefficients,
representative interpolation timeslots, per-object/channel contribution
energy, Qout energy, state/presence transitions, and provenance hashes. The
trace is fail-closed unless `--pcm --joc-gate6c` are both present. Its
independent reconstruction check recomputes every object/timeslot/subband as
`sum(Qin[channel] * interpolatedCoefficient)` and rejects a tampered Qout in
the built-in self-test; it does not change the production reconstruction
algorithm.

Using the existing bounded 5-second r4 fixtures (no DEE re-encoding and no
BEAR audio export), both cases completed 157 coded access units at config3;
Gate6C passed and every trace record reported `maxAbs=0`,
`relativeRms=0`, `mismatchCount=0`. The evidence is
`tmp/oracle/joc-matrix-trace-r2/single-object-01.jsonl` and
`single-object-02.jsonl`, with command logs beside them. The process exit is
1 because the existing Gate5C smoke status is `INCONCLUSIVE` below its
1000-AU coverage threshold; this does not invalidate the per-AU trace or its
Gate6C result. Gate6C's 158 batches include the terminal flush; 157 is the
actual bounded coded-AU count. Both fixtures report all 15 objects present in
all 157 AUs, so no absent-to-present transition was observed; the trace still
records the implementation's zero-output/clear-previous-state policy for
future absent-object fixtures.

These fixtures contain additional OAMD elements at the known
reserved/unsupported trim-warp boundary. Matrix-trace mode therefore reuses
the existing renderer-export compatibility policy: those additional elements
are excluded from the Gate6C metadata state while the standard B2A object
updates remain available. This policy is outside the Qin, interpolation, and
Qout calculations and does not affect the exact matrix invariant above;
additional OAMD semantics themselves remain unvalidated.

Compact per-case summaries (about 10 KB each) are
`tmp/oracle/joc-matrix-trace-r2/single-object-01.summary.json` and
`single-object-02.summary.json`; the larger JSONL files remain the raw
per-AU evidence. They are generated by
`scripts/summarize_joc_matrix_trace.py` (its self-test passes).

Reproduction commands were:

    build-mm\Release\Eac3AccessUnitProbe.exe tmp\oracle\single-object-oracle-bounded-5s-r4\ec3\single-object-01.ec3 --max-units 157 --oamd --joc --pcm --joc-gate6c --joc-table docs/dev/ts_103420_tables.c --joc-matrix-trace tmp\oracle\joc-matrix-trace-r2\single-object-01.jsonl --summary
    build-mm\Release\Eac3AccessUnitProbe.exe tmp\oracle\single-object-oracle-bounded-5s-r4\ec3\single-object-02.ec3 --max-units 157 --oamd --joc --pcm --joc-gate6c --joc-table docs/dev/ts_103420_tables.c --joc-matrix-trace tmp\oracle\joc-matrix-trace-r2\single-object-02.jsonl --summary
    py -3 scripts\summarize_joc_matrix_trace.py --input tmp\oracle\joc-matrix-trace-r2\single-object-01.jsonl --output tmp\oracle\joc-matrix-trace-r2\single-object-01.summary.json
    py -3 scripts\summarize_joc_matrix_trace.py --input tmp\oracle\joc-matrix-trace-r2\single-object-02.jsonl --output tmp\oracle\joc-matrix-trace-r2\single-object-02.summary.json
    scripts\validate-all.ps1 -BuildDir build-mm -Configuration Debug

Final validation was 3/3 PASS: unit tests, report schema, and smoke test.

The L/R fixture classification is complete at this layer: both matrix
summaries are diagnostically `distributed` (17 non-zero object/channel cells;
top-cell fractions `0.8185` and `0.8221`), while Qout is also distributed over
6 non-zero objects (top-object fractions `0.8145` and `0.8221`). For every
AU, the exact independent Qin/interpolation sum equals captured Qout within
the checker (`3,617,280` complex values, zero mismatches). This proves the
local matrix/Qout invariant, not source-program spatial equivalence or a
private Dolby slot identity. `present=0` was not observed in either fixture
and is not the current L/R cause; absent-state coverage remains a separate
test boundary.

## Status refresh: 2026-08-31 (EAR/BS.2127 polar mapping fix)

The E audit isolated a real adapter defect for six of eight deterministic
objects: the previous project conversion used Euclidean azimuths (`90/135`
degrees), while the pinned EAR 2.1.0/ITU-R BS.2127 sector conversion emits
`30/70/110` degrees (with the corresponding negative positions). The
dependency-free port in `tools/atmos-render/run_bear_montero_bundle.py` is
validated against the pinned EAR implementation, including center/top and
nonfinite fail-closed cases; a seeded 10000-point cube comparison had maximum
absolute numeric difference `0`, and the focused adapter suite is 4/4 PASS.

The fresh bounded normal-render proof is
`tmp/oracle/oamd-fixed-ear-r2/fixed-normal-summary.json`. With the same
decoded bundles, BEAR data, listener, 240000-frame window, latency handling,
and no normalization, Lm/Rm/Ls/Rs now measure maximum absolute ΔILD/ΔIPD of
`0.030/0.004`, `0.019/0.005`, `0.015/0.001`, and `0.014/0.002` dB/rad;
C/Mono remain `0.054/0.001` and `0.012/0.000`. This is a renderer-adapter
correction, not a Dolby private-slot mapping claim. L/R remain separately
non-equivalent (`29.964/3.104` and `15.255/1.235`) and are still
`INCONCLUSIVE` between JOC PCM decomposition and anonymous slot identity.

## Status refresh: 2026-08-31 (official EAR per-slot polar audit r4)

The decisive E counterfactual preserves every decoded slot's original OAMD
standardXYZ, active/gain, and timing, but replaces only the project's
Cartesian-to-polar conversion with the pinned EAR `point_cart_to_polar`
implementation. The same BEAR/default.tf/listener, 240000-frame window, and
no normalization are used. The exact official source-adapter positions are L/R
`+30/-30` degrees, Lm/Rm `+70/-70`, Ls/Rs `+110/-110`, C `0`, and Mono
`+90` elevation, all distance 1. The report is
`tmp/oracle/oamd-counterfactual-r4/oamd-counterfactual-summary.json`; each
case also records the standard position, official polar position, and the
per-AU trace.

E restores the side/surround spatial response: maximum absolute ΔILD/ΔIPD is
`0.030/0.004`, `0.019/0.005`, `0.015/0.001`, and `0.014/0.002` dB/rad for
Lm/Rm/Ls/Rs, compared with A's `21.446/3.102`, `16.654/2.891`,
`20.886/3.062`, and `19.011/3.113`. C and Mono remain near their A baseline.
L/R do not recover under E (`29.964/3.104` and `15.255/1.235`), so their
remaining discrepancy is separate and remains `INCONCLUSIVE` between JOC PCM
decomposition and anonymous slot identity. This is diagnostic evidence for
the pinned EAR/BEAR semantic boundary, not a claim that Dolby's private JOC
slot mapping is known. This earlier diagnostic result directly motivated the
minimal Python adapter correction recorded in the subsequent status refresh.

## Status refresh: 2026-08-31 (OAMD counterfactual audit)

The existing bounded r4 DEE/native bundles were audited without re-encoding.
Each `tmp/oracle/oamd-counterfactual-r1/single-object-*/au-slot-trace.json`
contains 158 AU rows (157 metadata AUs plus the 577-sample EOS tail), 15 slots
per row, PCM RMS/energy, 1-based `objectIndex`, active/gain/ramp, and standard
coordinates. AU0/AU1 are the center bootstrap; the first non-center update is
at source position 7680 (AU5). The tail is explicitly marked `metadataCount=0`
and carries the last metadata state for measurement only.

Three BEAR renders use the same default.tf, listener, 167-sample renderer
latency, 240000-frame window, and no normalization: A is the existing OAMD
render; B forces every slot to the source ADM object's Cartesian position; C
forces every slot to the front midpoint. The aggregate report is
`tmp/oracle/oamd-counterfactual-r1/oamd-counterfactual-summary.json`. B does
not materially restore source-BEAR spatial behavior: for C, Lm, Rm, Ls, Rs,
and Mono, B is byte/metric-equivalent to A because the energized slot already
has that position (C and Mono remain near the prior A baseline); for L and R,
B changes the result but worsens the endpoint metrics. C is generally worse.
This counterfactual is diagnostic only and is not a normative object mapping.

Compact max absolute ΔILD/ΔIPD (dB/rad) versus source-object BEAR is:

`A: L 15.748/2.606, R 18.116/3.116, C 0.054/0.001, Lm 21.446/3.102,
Rm 16.654/2.891, Ls 20.886/3.062, Rs 19.011/3.113, Mono 0.012/0`.

`B: L 27.045/3.108, R 14.489/3.071, C 0.054/0.001, Lm 21.446/3.102,
Rm 16.654/2.891, Ls 20.886/3.062, Rs 19.011/3.113, Mono 0.012/0`.

This initial A/B alone did not expose the coordinate-adapter bug because B
still passed its forced Cartesian position through the same project `polar()`
function as A. The later independent EAR-backed E audit above supersedes that
inference and confirms the adapter defect for the side/surround cases, while
L/R remain `INCONCLUSIVE`. No production C++ was changed. The new script has a provenance
and hash-scoped reuse marker plus self-test (`PASS`); `py_compile`,
`git diff --check`, and `scripts/validate-all.ps1 -BuildDir build-mm
-Configuration Debug` remain `PASS`.

## Status refresh: 2026-08-31 (bounded object-only source reference)

The controlled local derivative `tmp/oracle/powder-diagnostic-ear-compatible-preserve.wav`
(the original POWDER SNOW file remains untouched) was rendered for exactly
240000 samples through the official BEAR `BEAROfflineRenderDriver` ADM
selection and `convert_objects` adapter. The wrapper filtered the returned
rendering items to eight `Objects` and excluded exactly ten
`DirectSpeakers` items, corresponding to source tracks 1-10; the per-item
audit in `tmp/oracle/bear-source-object-only.json` reports zero peak for all
ten excluded tracks in the bounded window. The source bed peak is therefore
0.0, object input peak is 0.03999997, and no normalization was applied.

The source-object-only reference is `PASS`: 48 kHz, default identity listener,
the checked-in `docs/dev/reference-cache/bear-default.tf`, 167-sample
algorithmic-latency compensation, 240000 compared frames, and stereo output
peaks `0.22184929/0.23376507`. Output and execution evidence are
`tmp/oracle/bear-source-object-only-raw-f32.wav`,
`tmp/oracle/bear-source-object-only.json`, and
`tmp/oracle/bear-source-object-only.log`. This is a bed-zero equivalent
boundary and an official ADM-to-ObjectsInput reference; it is not a claim that
the local diagnostic derivative was Dolby-authored.

The existing complete 157-AU DEE/native bundle path is `PASS` for JOC-path
execution through BEAR export and bundle rendering. However, the deterministic
fixture's full-chain spatial/programme equivalence is `FAIL / NOT EQUIVALENT`.
The two endpoint renders were compared without independent normalization in
`tmp/oracle/powder-diagnostic-source-object-vs-joc.json`: per-tone L/R complex
gain (magnitude/phase), waveform correlation, lag, best scalar gain, residual
RMS/energy, peaks, and RMS are recorded for 401/503/607/709/811/919/1021/1129
Hz. The JOC/source-object peak ratios are L/R `1.1665/1.1533` and the RMS
ratios are L/R `1.1049/1.0721`. Per-tone ΔILD dB in frequency order is
`-10.470, -2.522, +0.001, +0.162, -1.121, -2.580, +1.273, -1.072`; ΔIPD
rad is `1.226, 1.983, 0, -0.488, 0.171, -1.766, -0.227, -0.378`.
The 607 Hz near-match alongside the other frequency-dependent differences
rules out a pure global-gain or alignment explanation. Root-cause attribution
remains `INCONCLUSIVE` between JOC PCM coding differences and OAMD/slot
mapping; slot identity is not used as a prerequisite for reporting the
programme result. The original unmodified source ADM remains subject to the
previously recorded EAR/BEAR compatibility and authoring limits.

## Status refresh: 2026-08-31 (eight single-object oracle cases)

Eight deterministic one-object derivatives were generated from the untouched
18-channel POWDER SNOW ADM. The corrected derivatives are physically bounded
to 240,000 samples (5.0 seconds); their RIFF/data sizes are rewritten, AXML
programme/object/block timing is bounded to 5.00000 seconds, and CHNA/DBMD
remain retained. Bed tracks 1--10 and seven of object tracks 11--18 are zero,
while one object track contains the same seeded wideband stimulus (seed
`20260831`, band-limited noise, 12 tones from 173 to 11789 Hz, and five gated
pulses). This is a local diagnostic derivative, not Dolby-authored content.
The corrected manifest is `tmp/oracle/single-object-cases-bounded-r3-manifest.json`.

The corrected complete execution matrix is `PASS` at its individual layers:
DEE `atmos_mezz` 448 kbit/s line/RF DRC-none encoding is 8/8 with 157 access
units per output; native OAMD B2A is 157/157 in all 8 cases, with Gate6C and
`bearExport` passing; official BEAR source-object rendering is 8/8 (eight
Objects, ten DirectSpeakers excluded, bed peak zero); and JOC bundle rendering
plus endpoint reports are 8/8. The aggregate summary, per-case hashes,
configs, logs, bundle paths, and reports are in
`tmp/oracle/single-object-oracle-bounded-5s-r4/single-object-oracle-summary.json`.
All eight physical inputs contain exactly 240000 frames/5.0 seconds and
12,960,000 data bytes; all eight EC3 outputs are 281,344 bytes and ffprobe
reports 5.024 seconds. The earlier
`tmp/oracle/single-object-oracle/single-object-oracle-summary.json` remains
historical only: its harness copied the full 448-second carrier, so its EC3
outputs must not be used as bounded-duration evidence. The one-case r3 proof
is retained under `tmp/oracle/single-object-oracle-bounded-5s-r3/`.

The native PCM slot matrix shows strong decoded-signal evidence without
assuming slot identity. The best anonymous slot correlations are 0.993 for
the L/R cases (tracks 11/12, decoded slots 2/1), 1.000 for center (track 13,
slot 1), and about 0.522 for the five side/surround/mono cases (tracks 14--18,
slot 1); the 12-tone frequency matrix is in each `slot-reports-rerun` report.
For cases 3--8, the selected slot's frequency ratios are approximately
0.975--1.001, while cases 1--2 split energy across anonymous slots. This
supports a mostly preserved codec-side PCM response in the bounded window,
but is not a slot-identity proof. The r4 per-case slot reports are under
`tmp/oracle/single-object-oracle-bounded-5s-r4/slot-reports-rerun/`.

At the final steady-state AU, every case has 15 active slots at 0 dB: 11
non-center coordinates and four centered slots. The centered slot IDs are
`1,13,14,15` for L, `7,13,14,15` for R, and `2,13,14,15` for C, Lm, Rm, Ls,
Rs, and Mono. The same 11 non-center coordinate values occur in every case,
but their slot permutation follows the encoded programme: this is not a
common coordinate-collapse or missing-position result. AU0 and AU1 are the
center bootstrap; exact 157-update sequences, including active flags and
coordinates, are retained per case below the oracle output root.

OAMD metadata is also present in all 157 updates per case. AU0/AU1 are the
center bootstrap; later updates carry non-center coordinates distributed across
slots 1--12 while slots 13--15 remain centered. The coordinate set is common
across cases, but the slot permutation follows the encoded case, so the
artifacts preserve the important distinction between metadata trajectory and
source-object identity; no one-source-to-one-slot mapping is inferred. The
complete per-slot position/time sequences are under
`tmp/oracle/single-object-oracle-bounded-5s-r4/slot-reports-rerun/`.

The eight source-object-vs-JOC endpoint comparisons are `FAIL / NOT
EQUIVALENT` as a programme-spatial baseline, not a listening-quality verdict.
Cases C and Mono are near-equivalent at the measured tone layer (maximum
absolute ΔILD/ΔIPD about `0.054 dB/0.001 rad` and `0.012 dB/0 rad`), while
the L/R, middle, and surround positions show frequency-dependent differences:
maximum absolute ΔILD/ΔIPD is respectively `15.748/2.606`, `18.116/3.116`,
`21.446/3.102`, `16.654/2.891`, `20.886/3.062`, and `19.011/3.113` for
L, R, Lm, Rm, Ls, and Rs. All per-tone complex gains/phases, waveform
correlation, lag, best scalar gain, residual, peak, and RMS values are in
`tmp/oracle/single-object-oracle-bounded-5s-r4/endpoint-reports/`.

For a compact per-case endpoint digest, the L/R RMS ratios, measured
wideband best-correlation lags, wideband residual RMS, and maximum absolute per-tone ΔILD/
ΔIPD are: L `0.895/0.755`, `-4246/257`, `0.003444/0.001934`,
`15.748 dB/2.606 rad`; R `0.925/1.090`, `201/213`, `0.002050/0.004416`,
`18.116 dB/3.116 rad`; C `1.000/1.000`, `256/256`, `0.002683/0.002998`,
`0.054 dB/0.001 rad`; Lm `1.043/1.202`, `253/-73`,
`0.004296/0.002001`, `21.446 dB/3.102 rad`; Rm `1.187/1.016`, `-71/253`,
`0.001765/0.004869`, `16.654 dB/2.891 rad`; Ls `0.833/1.051`,
`256/8382`, `0.003191/0.002085`, `20.886 dB/3.062 rad`; Rs
`1.168/0.840`, `4188/256`, `0.001937/0.003782`, `19.011 dB/3.113 rad`;
and Mono `1.001/1.001`, `254/254`, `0.002607/0.002797`,
`0.012 dB/0 rad`. The corresponding 12-value ΔILD/ΔIPD vectors are in each
endpoint JSON; no independent normalization was applied.

The root-cause result remains `INCONCLUSIVE`: the bounded r4 case matrix leans toward an
OAMD/slot-mapping or renderer-semantic issue for cases 3--8, but the L/R slot
split and lossy JOC path do not isolate that from JOC PCM coding. No concrete
parser defect was proven, and no production C++ was changed. The next useful
experiment is an authoritative per-AU object identity/renderer trace, keeping
PCM, OAMD, and endpoint layers separate.

The focused r4 mapping audit found no reproducible common off-by-one or callback
ordering defect. Source positions are L `(-1,1,0)`, R `(1,1,0)`, C `(0,1,0)`,
Lm `(-1,0,0)`, Rm `(1,0,0)`, Ls `(-1,-1,0)`, Rs `(1,-1,0)`, and Mono
`(0,0,1)`. The strongest anonymous PCM slots are respectively `2,1,1,1,1,1,1,1`
with correlations `0.993,0.993,1.000,0.522,0.522,0.522,0.522,0.522`;
L/R energy is split across the top three slots (`88.0/9.4/2.5%` and
`82.3/8.0/7.5%`), while cases C--Mono are effectively single-slot. For every
case, non-center OAMD begins at source position 7680 (after AU0/AU1 bootstrap);
the observed first coordinates are `(0.5,0,0)`, `(0.5,0,0)`, `(0.5,0,0)`,
`(0,0.5,0)`, `(1,0.5,0)`, `(0,1,0)`, `(1,1,0)`, and `(0.5,0.5,1)` for
L/R/C/Lm/Rm/Ls/Rs/Mono. Native code consistently treats objectIndex as
1-based and the BEAR adapter explicitly converts it once to zero-based; the
existing 15-object-plus-LFE contract is separately validated. This evidence
does not prove the anonymous slot identity, so no production parser change is
justified.

The corrected scripts pass their focused checks: analyzer RIFF-only boundary,
physical carrier construction, orchestration self-test, and `py_compile` all
`PASS`; `scripts/validate-all.ps1 -BuildDir build-mm -Configuration Debug`
passes unit tests (10 suites), report schema, and smoke (`3/3`).

## Status refresh: 2026-08-31 (initial deterministic ADM/JOC oracle boundary; superseded by object-only reference)

The public EBU two-language ADM download remains a useful RIFF/WAVE ADM/BWF
framing fixture only. With the original file, DEE 5.2.1 first fails while
opening the Atmos master with `ATMOS_STORAGE_RES_FORMAT_INVALID`; the EAR/
track-field-completed derivative then reaches the separate authoring gate and
fails `Content was not authored with Dolby tools`. No Dolby-authored object
master was found locally or made from it. The existing
`media/POWDER SNOW Live V9.8.6.wav` lineage is a RIFF 18-channel file with ten
DirectSpeakers bed tracks followed by eight one-channel Objects tracks; its
ADM positions are static corner/center coordinates in
`tmp/oracle/powder-adm-audit.json`.

For a bounded PCM oracle, `scripts/make_adm_diagnostic_carrier.py` created the
ignored derivative `tmp/oracle/powder-diagnostic-5s.wav` without modifying the
source, preserving JUNK/fmt/axml/chna/dbmd bytes. It zeros bed tracks 1--10
for the first 240,000 frames and inserts eight deterministic gated tones
(401/503/607/709/811/919/1021/1129 Hz) plus distinct pulses on tracks 11--18.
The derivative SHA-256 is
`064f42b21efbd4d12fc7211eec2c9e1a2f6af4c4878647cc145b75a4685ef5dd`; the
manifest and audit are `tmp/oracle/powder-diagnostic-carrier.json` and
`tmp/oracle/powder-diagnostic-audit.json`.

DEE 5.2.1 successfully encoded that derivative through the supported
`atmos_mezz` + `encode_to_atmos_ddp` workflow with line/RF DRC profiles set to
`none`; the output is
`tmp/oracle/powder-diagnostic-5s-448k.ec3` (SHA-256
`8C7348D1320035843110B0FC5DEFC6EFDD16EA3F8E0DCCD87445310724ECB4C2`). The
native command
`build-mm\Debug\Eac3AccessUnitProbe.exe tmp\oracle\powder-diagnostic-5s-448k.ec3 --max-units 100 --oamd --joc --pcm --joc-gate6c --joc-bear-export tmp\oracle\powder-diagnostic-joc-bear-100`
passed B1/B2A/B2B syntax, 15 dynamic objects plus the LFE helper, Gate6C
finite PCM/range checks, and BSCN export. The bounded QMF smoke is
`INCONCLUSIVE` only because 100 access units do not meet its 1,000-unit
coverage threshold. The anonymous slot correlation/gain/lag and 15x8 tone
response report is `tmp/oracle/powder-diagnostic-ab-report.json`, reproducible
with `scripts/analyze_joc_object_bundle.py`.

The initial “all-center” observation was too broad. The read-only bit-level
audit `tmp/oracle/powder-diagnostic-b2a-deep.json` repacks the element body
exactly as `oamd-b1.cpp::copyBits` and records every field/cursor. Both
`Syntax5511Lsb` and `Table31Msb` produce the same valid records: AU0 has the
center bootstrap, while AU1 carries explicit non-center absolute codewords
(for example object records x=31, y=31, x=62 and z=15). The same audit on
`tmp/oracle/montero-b2a-deep-100.json` first sees non-center records at AU8.
The native bundle metadata agrees with those later records. The generic
`oamd-b2b-selftest.txt` also passes 37/37 cases, including non-center absolute
position conversion. Therefore a generic bit-order, B2A, B2B, or JSON-export
loss is not evidenced.

At this initial checkpoint, the remaining issue was an authoring/semantic
boundary rather than a proven parser bug: the DEE `atmos_mezz` output's
startup/default and later slot-position sequence was not independently proven
to preserve the eight source ADM object identities (and the prior `<adm>`
vector was center-only). The later object-only reference at the top of this
file supersedes that temporary source-reference block and establishes the
programme-level non-equivalence result. Keep this checkpoint's deterministic
PCM/slot-frequency result as `PASS` at its layer; do not claim Dolby-authored
provenance or infer a production parser change from it.

The reusable RIFF-only scripts were self-tested with
`tmp\reference\ear-2.1.0\venv\Scripts\python.exe scripts\test_adm_joc_oracle.py`
(`admJocOracleSelfTest=PASS cases=3`); BW64/RF64 is explicitly unsupported
because these diagnostics do not resolve `ds64` chunk sizes.

## Status refresh: 2026-08-31 (bounded programme-level BEAR A/B; superseded by object-only reference)

The deterministic carrier was bounded to the first 240,000 samples in the
ignored `tmp/oracle/powder-diagnostic-bounded-adm.wav`, retaining its
`axml/chna/dbmd` metadata and leaving the source untouched. The official BEAR
`bear.render_cli` path was invoked with the same 48 kHz input programme
`APR_1001`, identity listener, and `docs/dev/reference-cache/bear-default.tf`,
but rejected the ADM before rendering with
`audioStreamFormat AS_00011001 has a reference to both an audioPackFormat and
an audioChannelFormat`. The official EAR `render_file` path with
`--enable-block-duration-fix`, and the documented `ear-utils regenerate`
compatibility attempt, report the same structural error. Evidence is in
`tmp/oracle/bear-adm-render.log`, `tmp/oracle/ear-adm-render.log`, and
`tmp/oracle/ear-regenerate.log`; no metadata marker was fabricated.

The DEE side was rerun to the complete 157-AU boundary with
`tmp/oracle/powder-diagnostic-native-157.txt`: OAMD B2A is 157/157 PASS,
Gate6C reconstructs 157 access units, `bearExport=PASS batches=158
metadata=2355`, and the LFE reconstruction peak is zero. The official BEAR
bundle consumer then rendered the complete 5.024-second bundle using the same
`default.tf`; after the empirical 167-sample path compensation, the report
compares the first 240,000 samples. The raw stereo render is under
`tmp/oracle/bear-bundle-render-157/` (peak L/R `0.25879475/0.26959410`, no
normalization, LFE excluded).

`tmp/oracle/powder-diagnostic-programme-ab.json` records per-tone L/R complex
gain (magnitude/phase), waveform correlation, lag, best scalar gain, residual
RMS/energy, and peak/RMS summaries for the eight deterministic object tones.
Its result is `INCONCLUSIVE`: the BEAR bundle side is a valid PASS at its own
layer, but the source-programme A side never rendered because the official
ADM parser rejected the source. This is an input-compatibility boundary, not
a slot-identity prerequisite; slot identity remains separately unknown and
is not used to suppress the bundle response measurements.

## Status refresh: 2026-08-31 (EAR-compatible ADM retry; superseded by object-only reference)

Local source inspection found no `extra_pack_ref`, ADM dual-reference
compatibility switch, or existing fix in EAR 2.1.0 or the pinned BEAR source.
EAR's `--enable-block-duration-fix` only addresses audioBlockFormat timing;
the earlier `ear-render`/`regenerate` attempts therefore could not affect the
dual `audioPackFormat`/`audioChannelFormat` error.

As a bounded compatibility attempt, the ignored AXML derivative removed only
the redundant `audioPackFormatIDRef` child from 18
`audioStreamFormat` elements that already retained an
`audioChannelFormatIDRef`. `ear-utils replace_axml` produced a parseable
file, but rewrote `JUNK` and dropped `dbmd`; that output is retained only as
utility evidence. The final input
`tmp/oracle/powder-diagnostic-ear-compatible-preserve.wav` was built by a
chunk-preserving temporary remux: only AXML differs; `fmt`, `data`, `chna`,
`dbmd`, and `JUNK` hashes are unchanged. Counts, XML SHA-256, chunk hashes,
and the utility result are in
`tmp/oracle/powder-diagnostic-axml-ear-compatible.json`,
`tmp/oracle/powder-diagnostic-ear-compatible-preserve.json`, and
`tmp/oracle/check_compat_chunks.py` output. The original carrier and source
audio were not changed.

EAR 2.1.0 strict `render_file -s 0+2+0 --programme APR_1001
--enable-block-duration-fix` accepts the preserved derivative and emits the
stereo reference (`tmp/oracle/ear-compatible-preserve-render.log`). The
official BEAR `bear.render_cli` then accepts the ADM structure but reaches
the next unsupported input boundary: `DirectSpeakerCartesianPosition` has no
`azimuth` in BEAR's `convert_direct_speakers`. Its documented
`--apply-conversion to_polar` converts Objects only, not DirectSpeakers;
there is no further documented compatibility transform to apply, so no free
XML or official-source change was made. The exact result is in
`tmp/oracle/bear-source-adm-preserve-render.log`.

The complete DEE 157-AU bundle remains independently rendered by official
BEAR with the same `default.tf`, 48 kHz, identity listener, and 167-sample
path compensation. `tmp/oracle/powder-diagnostic-programme-ab.json` remains
`INCONCLUSIVE`: its bundle side is PASS and contains the per-tone L/R complex
gain/phase, correlation, lag, best gain, residual, and peak metrics, while a
valid official BEAR source-programme binaural output is unavailable after
the documented compatibility boundary. This is not blocked by unknown slot
identity; it is blocked by the remaining DirectSpeakers Cartesian API gap.

## Read-only config3 phase/DRC audit (2026-08-30; bounded)

The MONTERO native audit processed 4,306 access units as config3 with 15
dynamic objects plus the LFE bypass. It found no lost units, fallback,
metadata/object discontinuity, non-finite reconstruction, or layout mismatch;
the evidence is in `tmp/reference/montero-bear-export-v2.log` (config3 paired
4,306/4,306, max continuous run 4,306). The current BEAR export used
`eac3DecodeDrcScale=1` and `cons_noisegen=0`.

ETSI TS 103 420 V1.2.1 Table 47 labels config3 as 5.X with a 90-degree phase
shift and Table 48 gives it five channels. Clause 6.6/Pseudocode 7 only feeds
decoded QMF through the reconstruction matrix; it specifies no additional
decoder phase transform. Accordingly, the current config0/config3 identity and
QMF path is not a confirmed phase bug. This does not independently prove the
encoder's preprocessing. A direct config0-vs-config3 numerical QMF oracle is
still a useful future test.

A 32-second FFmpeg MONTERO decode A/B was generated under
`tmp/reference/drc-montero/`: `montero-drc0.f32` has SHA-256
`7C8F4BF90DC761973F251D327E02AD41177852A6A6FF723B95673C6EE76B50B8`, peak
`0.44134897`, RMS `0.02673527`; `montero-drc1.f32` has SHA-256
`451B0A44B487654D67BC88DC526A2425B5EEACC4739E6928283CF5CD93EA7977`, peak
`0.44134897`, RMS `0.02707143`. The arrays differ (max difference
`0.04644170`, difference RMS `0.000746186`). Therefore the reference-
equivalence baseline should be rerun with `drc_scale=0` before attributing
listening quality to the renderer; DRC alone does not prove a JOC matrix bug.
Program completeness remains `INCONCLUSIVE` without an independent object
reference.

The same full-flow DRC0 run used the explicit command path with
`--eac3-drc-scale 0 --eac3-cons-noisegen 0` and wrote a fresh bundle at
`tmp/listening/bear-montero-drc0/work/export3`; it completed with 4,305
contiguous batches, 64,560 accepted metadata blocks, and 6,610,944 object
frames. The consumer output and provenance are under
`tmp/listening/bear-montero-drc0/`. Compared with the DRC1 bundle, object PCM
diff RMS was 0 for movement, 0.000285061 (0.77% relative) for vocal, and
0.001329224 (2.72% relative) for dense; corresponding object max differences
were 0, 0.01524186, and 0.03250784. Final stereo raw-f32 max/diff-RMS were
movement `0/0`, vocal `0.02308065/0.001088427`, and dense
`0.05100453/0.004986129`. Movement is identical, so DRC is not the main
cause of the observed renderer difference.

The clip-gain diagnostic report
`tmp/reference/montero-clipgain-scan.json` sampled 4,303 of the 4,306 native
payload-14 records (the native association count is 4,306); every sampled
value was `x=3,y=7`, giving `joc_clipgain=1.109375`. TS 103 420 defines the
field and value formula but provides no Qout/object-PCM application formula in
§6.4/§6.6, so the current parser omission is a diagnostic omission, not a
confirmed audio bug. JOC indexing, gain handling, dequantization,
interpolation, and QMF currently have no confirmed issue; completeness remains
limited by the independent object oracle, reserved metadata, and renderer
boundaries.

## Phase 5 BEAR MONTERO listening boundary (2026-08-30; PASS, open-reference)

The full official BEAR `default.tf` is now cached at
`docs/dev/reference-cache/bear-default.tf` (235,633,612 bytes, SHA-256
`c23a36289f246c96779fdce75e108187185d3ec7aeedd6afa25f7c3dc5e42131`). The
input `media/01. MONTERO (Call Me By Your Name).m4a` is confirmed by ffprobe
as an E-AC-3 JOC/Atmos stream (48 kHz, 6-channel 5.1(side), duration
137.768313 s); its SHA-256 is
`f2a24840836410fed10c875fe1648f51f7191722aabd1a5a7c3c1d8a1d387a27`.
The stereo ALAC input was not used.

The Gate6C export bridge processes all 4,306 access units causally and writes
4305 contiguous v2 batches at
`tmp/listening/bear-montero/work/export9`. The official BEAR consumer produced:
`tmp/listening/bear-montero/MONTERO-BEAR-open-reference-object-movement-raw-f32.wav`
(10 s), matching `*-stable-vocal-raw-f32.wav` (15 s) and
`*-dense-complex-raw-f32.wav` (11 s), plus `*-audition-minus2dB-s24.wav` copies.
Provenance is in `tmp/listening/bear-montero/provenance.json`; flags prove
official BEAR, no BBC BRIR, no ALAC, and no normalization. The output is an
open-reference BEAR render, not a Dolby-equivalence claim. Reserved
`warp_mode=3` is explicitly not rendered; LFE is accounted for and excluded
per Tech 3396. The actual-update queue accepted 64,560 deduplicated blocks,
mapping `rampDuration` to BEAR `interpolationLength`; source windows compensate
an empirical 167-sample latency. Raw float32 is unmodified; audition copies
use one fixed -2 dB gain.

Final validation: `scripts\\validate-all.ps1 -BuildDir build-mm -Configuration
Debug` — PASS (unit tests, report schema, smoke test).

The narrow reuse attempt confirms the exact missing boundary: `--dump-eac3`
retains the 4,306 compressed units at
`tmp/listening/bear-montero/work/montero.eac3`, but
`Eac3NativeConfig4SceneProbe --joc-config 3 --max-aus 4306` rejects that dump
before its first prepared-scene batch (`batches=0`). Gate7c currently keeps
object PCM and metadata only in process-local metrics and does not serialize
them. This historical failure is superseded by the Gate6C export bundle.

Earlier gate7c endpoint and dump-reuse failures remain historical provenance
(`tmp/reference/montero-joc-gate7c.log`); they are superseded by Gate6C export.

This is the current status tracker for the native E-AC-3/JOC decoder, the
renderer-neutral scene/panner path, SOFA/BRIR work, and the planned stereo
headphone route. Durable architecture and execution order remain in
`docs/dev/eac3-joc-full-chain-plan.md` and
`docs/dev/eac3-joc-next-roadmap.md`; this file records the current evidence
boundary and next implementation slice.

## Status refresh: 2026-08-30 (BEAR runtime boundary resolved; BEAR/EAR baseline)

The earlier dependency blocker is superseded by a successful isolated retry.
Official Boost 1.85.0 was built with the VS18 x64 developer environment and
explicit b2 `vcvarsall.bat` setup; see `tmp/reference/boost-build-shared.log`.
VISR 0.13.0 configured, built, and installed to ignored
`tmp/reference/VISR-install`; evidence is in
`tmp/reference/visr-configure-shared.log`, `tmp/reference/visr-build.log`,
and `tmp/reference/visr-install.log`. Pinned `visr_bear` configured and built
with explicit VISR/Boost paths (`tmp/reference/bear-configure-installed-2.log`
and `tmp/reference/bear-build.log`).

The deterministic official BEAR probe is
`tmp/reference/bear-front-center-impulse.json` with run log
`tmp/reference/bear-front-center-run-2.log`: a front-center object impulse
produced finite 2x512 binaural PCM, peak per ear
`[0.0407501757144928, 0.03817449510097504]`, 1023 non-zero samples, and
sum-of-squares `0.017260704189538956`. This proves the pinned source,
dependencies, data load, Python API, and actual PCM gates for one synthetic
case only; it does not establish Dolby equivalence or production integration.
The checked-in wrapper `tools/atmos-render/run_bear_front_center_probe.py`
passed with explicit local BEAR/VISR/DLL/data paths. The multi-position
project-vs-BEAR comparison is recorded below.

That comparison is now available at ignored
`tmp/reference/bear-boundary-comparison/report.json`, generated by
`tools/atmos-render/compare_bear_system_h_brir.py`. It covers front, left,
right, rear, upper, lower, and interior positions over 16,384 frames, with
48 kHz onset is peak-relative -60 dB; direct is onset..onset+255, late is
onset+1024..end, and tail is peak-relative -80 dB. All cases are finite and
nonzero. For front, project energy/peak are `1.653130`/
`0.343314,0.344832`, versus BEAR `0.018877`/`0.040750,0.038174`;
project onset/late-direct/late-total are `269`/`0.042075`/`0.037005`, versus
BEAR `167`/`0.017003`/`0.015166`. Project tail is truncated at frame 16,383,
whereas BEAR stops at 2,880. This supports the bounded inference that the
current BBC room BRIR has materially more late energy and a longer observable
tail than BEAR for front-center; it is not a Dolby-behaviour claim.

The three exact artifacts written by the upstream install were moved to
ignored `tmp/reference/site-packages-quarantine`; their external paths were
verified absent and no unrelated packages were touched. The wrapper still
requires the host Python interpreter and NumPy (these are not claimed to be
fully hermetic).

The ignored local reference cache now contains the exact BEAR revision named by
EBU Tech 3396 (`v0.0.1-pre`, commit `5eececb2c2671711c1f63a872e706a538a1d4a5a`),
the official EBU EAR 2.1.0 archive (tag commit
`a0e37d33f55ae7080b1aaccbce655680319f92ae`), and the official ITU BS.2127
companion ZIP. `Expand-Archive` and `tar -tf` succeeded for all three; exact
sizes, SHA-256 hashes, source URLs, and license boundaries are recorded in
`docs/dev/eac3-joc-reference-manifest.md`. The ITU ZIP root is labelled
`BS2127-0`, so it is an oracle/reference package but not proof of a BS.2127-1
source revision. No renderer code or runtime path was changed, and no
production Dolby/JOC equivalence claim follows from these archives.

Evidence correction and Phase 3 dependency attempts: the separate
`tmp/reference/visr-configure.log` is the VISR configure log and fails at
`find_package(Boost)` because the required Boost components are unavailable;
the separate `tmp/reference/bear-configure.log` is the `visr_bear` configure
log and fails earlier at `find_package(VISR)` because VISR was not installed.
These failures must not be conflated. They are historical first attempts and
are superseded by the successful developer-environment retry recorded in the
runtime-boundary refresh above. The original logs remain useful provenance:
`tmp/reference/boost-build.log` records the uninitialized-toolset attempt,
while `tmp/reference/visr-configure.log` and `tmp/reference/bear-configure.log`
retain the separate pre-install configure failures.

The pinned EAR 2.1.0 source is now runnable in the ignored local venv under
`tmp/reference/ear-2.1.0/venv`. `tools/atmos-render/ear_bs2127_oracle.py`
automatically invokes the focused local C++ panner probe and EAR's documented
`point_source.configure(bs2051.get_layout("9+10+3").without_lfe)` API. The
seven-case comparison report is `tmp/reference/ear-bs2127-comparison.json`:
`result=PASS`, `mismatchCount=0`, and `maxGainError=4.916495324747139e-7` at
the default absolute tolerance `1e-6`; all vectors are finite and power
normalized (maximum power difference `2.22e-16`). Cases cover front, left,
right, rear, upper, lower, and an interior point. Project speaker order is
the 22-channel System H order emitted in the report; project coordinates
`[front,left,up]` are converted to EAR/ADM `[right,front,up]` as `[-y,x,z]`.
This establishes a numeric BS.2127 point-source oracle only; it does not
establish BEAR's binaural/BRIR stage or Dolby JOC equivalence.

The official BEAR checkout, recursive source submodules, VISR 0.13.0 source,
and pinned `default_small.tf` remain available under ignored `tmp/reference`.
The historical WSL/Docker checks and pre-install failures are retained as
provenance only; they do not describe the current runtime state. Current
configure/build/PCM evidence is the successful boundary above.

## Status refresh: 2026-08-30 (Gate 6B2C OAMD additional elements)

Gate 6B2C now parses the real B1 `oa_element` containers for ID-2
`trim_element` and ID-5 `extended_object_element`; ID-1's per-object
`additional_table_data` remains a separate reserved field.  The parser uses
the B1 element's exact `rawBodyBits` boundary and dispatches duplicate,
version, reserved-codeword, truncation, and non-zero-tail failures without
committing partial state.  It implements TS 103 420 5.5.12, 5.5.13, 5.5.14,
and 5.5.15, including `NUM_TRIM_CONFIGS=9`, divergence table/code/reuse,
inactive/helper suppression, per-object trim disable, and extended position
codewords.  B2B applies the parsed values transactionally; extended precision
updates the effective coded position while preserving standard-precision
differential history.  The typed scene contract carries the precision
codewords but no new renderer math is claimed.

The B1 framing layer now applies TS 103 420 5.6.4.5: an unknown
`oa_element` is skipped only when its validated bounded element span carries
`b_discard_unknown_element=1`; unknown nondiscardable IDs remain
`Unsupported`, and known-element reserved or malformed syntax is never
masked by the discard flag.

The synthetic `Eac3OamdAdditionalProbe` passes 14 cases covering marker values,
multi-object/two-block dispatch, reuse with and without prior state, reset
boundary, reserved warp, truncation, non-zero tail, and empty-on-failure.
On the real MONTERO extracted E-AC-3 prefix, B1 reports ID-2 on 3/3 payloads
(7 body bits, first byte `0xc0`) and no ID-5; the normative parser rejects the
reserved `warp_mode=0b11` fail-closed, so B2B additional application is
`INCONCLUSIVE` (0 applied, 3 rejected).  This is evidence of sample presence
and reserved wire content, not a claim that the sample carries valid trim
settings.

Independent ownership audit against TS 103 420 Table 26 confirms
`oa_element_id_idx=1` is `object_element`, `2` is `trim_element`, and `5` is
`extended_object_element`; IDs 0, 3, 4, and 6--15 are reserved. Table 26 is
therefore consistent with the B1 parser's 4-bit ID dispatch. Table 26 and
5.5.4 also define `oa_element_size` as the total byte span of optional
alternate ID, discard flag, `oa_element`, and padding. The B1 reader starts
the element reader at that span, consumes the optional 4-bit alternate ID and
1-bit discard flag, and copies the remaining bits as `rawBodyBits`. For
MONTERO, size=1 means exactly 8 declared element bits minus the discard bit =
7 body bits; observed first byte `0xc0` is therefore body `1100000`, not a
byte-alignment or ownership error. The reserved `warp_mode=3` result remains
fail-closed.

Focused commands:

`cmake --build build-mm --target Eac3OamdB1Probe Eac3OamdAdditionalProbe Eac3OamdB2bProbe Eac3NativeBsiProbe Eac3AnnexHHeadphoneProbe Eac3AccessUnitProbe --config Debug -- /m:4`

`build-mm\\Debug\\Eac3OamdB1Probe.exe` -> `PASS` (15 cases), including
known/discardable-unknown/known continuation and bounded length rejection;
`build-mm\\Debug\\Eac3OamdAdditionalProbe.exe` -> `PASS` (14 cases);
`build-mm\\Debug\\Eac3OamdB2bProbe.exe` -> `PASS` (37 cases);
`build-mm\\Debug\\Eac3AnnexHHeadphoneProbe.exe` -> `PASS` (24 cases),
with exact 32 early and 32 late chunks, raw-24-bit marker checks, AC-3 and
six-block E-AC-3 outer-carrier routing, stream/timestamp continuity, seek/
cancel generation handling, and parity/reassembly failure cases;
the MONTERO command is `Eac3AccessUnitProbe.exe
tmp\\montero-ab\\full-native\\01.-MONTERO-Call-Me-By-Your-Name-native-copy.ec3
--max-units 3 --oamd` and reports B1 `ID 2:3`, B1 pass `3`, B2A pass `3`,
and B2B additional failure `3` with reason `reserved-trim-warp-mode`.
The same command with `--emdf` reports `emdfAnnexHPresence=ABSENT` and
`emdfAnnexHPayloadBytes=0`.

The remaining boundary is renderer policy: trim configuration selection and
trim/balance gain math are carried as typed metadata only.

The native TS 102 366 BSI parser now exposes `lfemixlevcode` and
`lfemixlevcod` from the real `mixmdate && lfeon` branch, including absent-flag
semantics and the complete 5-bit range. The BSI probe reports presence,
absence, range, and cross-frame stability. I0 remains `NO_MIX`: E.2.9 permits
the LFE-to-L/R mix only for 1/0 or 2/0 output with LFE output disabled, while
the current I0 contract is a 22-speaker BRIR bus plus a separate LFE sideband;
neither output-mode nor LFE-disabled programme ownership is proven. The I0
report now records this decision and reason explicitly.

Annex-H EMDF payload ID `0x7` now has a bounded H.3.7 header/presence parser
and serialized BRIR reassembler. It checks channel count, per-channel and LFE
gain presence, sequence start/end, BRIR chunk bounds, propagation delay, RT60
band validity, zero padding, start/end ordering, exact 32 early/late chunks,
and parity `XOR(bytes) ^ 0xA9`. The renderer-neutral contract retains raw
24-bit coefficient words without guessing signed fixed-point scale; no
headphone renderer is connected. The generic access-unit scan now emits
explicit AU ordinal, sample timestamp, frame type, substream ID, parse status,
and completion fields while feeding bounded payload bytes into a per-stream
cache/router; the supplied MONTERO and POWDER prefixes remain `NO_DATA` /
`ABSENT`.

## Status refresh: 2026-08-29

The project has a substantial diagnostic chain and bounded native config-3
bridge, but it does not yet have a production self-rendered Atmos route. The
production player still uses the existing FFmpeg/libav PCM path and, for the
current E-AC-3 eight-channel case, an eight-channel static 5.1.2 Windows
Spatial Audio bed. The dynamic-object Windows path is diagnostic only.

The shortest route to the first self-rendered output remains:

```text
bounded native config-4 PCM + JOC/OAMD prepared scene
  -> R2C2 sample-accurate Cartesian System H speaker bus
  -> I0P BRIR/LFE/output-contract preflight (diagnostic PASS)
  -> I0 raw EB3 -> stereo WAV/report
  -> I1 stereo WASAPI lifecycle
  -> I2 native M4A packet/trim/seek
  -> I3 production/release gate
```

R2B2 is a parallel/deferred renderer item for non-zero or fractional-delay
assets. It is required before broader, mature renderer acceptance, but it does
not block the current all-zero-delay BBC asset from reaching I0. A reusable
production decoder/session remains an I3 requirement, not a prerequisite for
the probe-local first-audible I0 artifact.

## Layered progress

### Decoder and native PCM

- **PASS, bounded:** N0 feature inventory, N1A state snapshots, and N2A1/N2A2/
  N2A3 ordinary-uncoupled spectral coefficient reconstruction on the supplied
  config-3/config-4/DEE prefixes. Advanced coding branches remain structured
  `Unsupported`.
- **PASS, reference only:** N3A scalar IMDCT/overlap temporal packing.
- **INCONCLUSIVE, diagnostic only:** N3B ordinary-uncoupled PCM session. It is
  not generic E-AC-3 PCM acceptance.
- **PASS, bounded:** N4A identifies the supplied raw config-4/DEE topology as
  Legacy AC-3 SID0 plus immediately following dependent E-AC-3 SID0. This is
  inventory/association evidence, not a native dependent-substream decoder.
- **PASS, config-3 only:** N5A/N5B native ordinary core and callback seam,
  with 1,536-sample AUs, coded channel IDs, LFE separation, reset/poison
  handling, and a separate 256-sample EOS tail. Their raw config-4 input is
  still rejected at the Legacy AC-3 frame.
- **PASS, bounded probe-local/native diagnostic association seam:** N5C accepts
  exactly the supplied Legacy AC-3 SID0 base followed immediately by
  dependent E-AC-3 SID0, emits a 1,536-sample AU contract with stable
  base/dependent channel identities and explicit LFE ownership, and fails
  closed on ordering/topology/callback errors. It deliberately reports
  `PCM_UNAVAILABLE_LEGACY_AC3`; no reusable production API or Legacy AC-3
  coefficient/IMDCT decoder was invented in this slice.
- **PASS, bounded coefficient-only probe-local/native frontend seam:** N5D adds
  `Eac3NativeLegacyAc3FrontendProbe` for exactly one complete 48 kHz, acmod=7,
  LFE, six-block Legacy AC-3 SID0 frame. It parses the Legacy BSI boundary,
  enters a conservative uncoupled audblk path, and reuses the checked-in
  bounded reader, exponent, bit-allocation, and mantissa cursor primitives.
  It records a deterministic complete six-block/per-channel coefficient-cursor
  boundary with one shared grouped-mantissa cursor per block across FBW and
  LFE, but does not expose coefficient values, IMDCT, overlap, PCM, DRC,
  JOC, or production API. The supplied raw config-4 frame and all six DEE
  first-base checks pass this `COEFFICIENT_ONLY` boundary: the raw file ends
  at bit 20456; the 1152k DEE vector at bit 18413; the other five at bit
  20456. Delta REUSE/NEW modes and other unsupported branches remain
  fail-closed.
- **PASS, bounded coefficient-value materialization:** N5E extends the same
  probe-local/native seam to expose finite renderer-neutral transform
  coefficient vectors with exact per-channel/LFE sizes and deterministic
  digests. It reuses the checked-in mantissa primitive and
  `ReferenceDitherSource`, retaining one dither stream per channel across the
  six blocks and one grouped BAP cursor per block across channels. The raw
  config-4 frame and all six DEE first-base checks pass with 6,552 values
  (`5 * 217 + 7` per block); this remains coefficient-only, with no IMDCT,
  overlap, PCM, DRC, JOC, dependent assembly, or production API.
- **PASS, diagnostic PCM only:** N5F connects those accepted vectors to the
  existing `Eac3TransformChannel` IMDCT/overlap primitive, keeping one
  transform state per coded channel. The six-block Legacy base emits exactly
  1,536 finite samples per channel and a separate 256-sample EOS tail, with
  long/short selection driven only by the FBW block-switch bits. This is a
  single-base-frame diagnostic result; no dependent assembly, JOC, production
  route, or production PCM acceptance is claimed.
- **PASS, dependent diagnostic PCM only:** N5G reuses the native E-AC-3
  coefficient and transform primitives for the immediately-following supplied
  dependent SID0 frame, strictly limited to 48 kHz, six blocks, `acmod=5`,
  `chanmap=0xA010`, and four weighted channels. It emits stable `L`, `R`,
  `VHL`, and `VHR` identities with 1,536 finite AU samples plus a separate
  256-sample EOS tail; for the first AU only, its ordinal sample start is
  `baseSampleStart=dependentSampleStart=0`. This remains a probe-local
  diagnostic seam: no base/dependent PCM
  assembly, JOC, renderer, production API, or production acceptance is
  claimed.
- **PASS, first-AU config-4 PCM assembly only:** N5H combines the accepted
  N5F Legacy base and N5G dependent owned PCM vectors into one ordered ten-channel
  diagnostic AU: `base.FL,FC,FR,SL,SR,LFE` followed by
  `dependent.L,R,VHL,VHR`. It preserves both source prefixes without mixing
  or replacing base `FL/FR`, checks 1,536 AU samples plus separate 256-sample
  tails per channel, and checks equal first-AU ordinal starts. This remains the
  stateless first-AU baseline; N5I adds the separate stateful multi-AU owner.
  No production API, JOC, renderer, or playback route is claimed.
- **PASS, diagnostic stateful three-AU config-4 assembly:** N5I adds a
  probe-local owner that holds the N5F/N5G dither sources and IMDCT/overlap
  channels across adjacent AU pairs. The supplied raw fixture and six DEE
  vectors each deliver three owned ten-channel AUs at starts `0`, `1536`, and
  `3072`; intermediate AUs have no EOS tail and the final EOS emits one
  independent 256-sample tail per channel. This proves the bounded diagnostic
  state seam only; parser syntax state remains frame-local and production
  acceptance is still `INCONCLUSIVE`.
- **PASS, explicit frame-local syntax ownership:** N5J adds a probe-local
  frame boundary owner that begins and completes each base/dependent pair in
  ordinal order, rejects active/reused or poisoned boundaries, and resets
  explicitly. The N5I dither/IMDCT-overlap state remains cross-frame; BSI,
  exponent, bit-allocation, and grouped-mantissa syntax state is rebuilt within
  each frame because its reuse scope is block/frame-local in the accepted
  parser. This closes the syntax-state-open item without claiming a reusable
  cross-frame syntax decoder.

Evidence: `docs/dev/eac3-joc-next-roadmap.md:227-310`, `:312-430`, `:430-508`,
`:653-678`, and `:693-715`; `docs/bug/media-foundation-status.md:481-587`;
focused targets
`build-mm/Debug/Eac3NativeConfig4CoreProbe.exe`,
`build-mm/Debug/Eac3NativeLegacyAc3FrontendProbe.exe`, and
`build-mm/Debug/Eac3NativeDependentPcmProbe.exe`, plus
`build-mm/Debug/Eac3NativeConfig4PcmAssemblyProbe.exe`.

### JOC and metadata

- **PASS, bounded config-3:** J0A1 extracts EMDF payloads 11/14, J0A2 applies
  fail-closed OAMD/JOC qualification, and J0A3 joins N5B with the existing
  JOC/QMF/OAMD/Gate 6C session. The bridge produces finite 15-object/LFE
  batches and timed metadata.
- **PASS, config-4 preflight:** J0A4 proves that the supplied raw EB3 and
  six DEE vectors carry payload 11/14 on the immediately-following dependent
  SID0 frame, with no strictly accepted EMDF container or target payload 11/14
  in the base frame. JOC payload14
  qualifies as config 4 with 7 channels; J0A4 only associates this result with
  the N5J topology/timeline and does not consume, transform, drop, or mix its
  ten PCM channels.
- **PASS, config-4 mapped session input (diagnostic):** J0A5 applies the
  normative mapping to the actual N5J owned vectors: dependent L/R, base
  FC/SL/SR, dependent VHL/VHR as the seven config-4 slots, with base LFE
  bypassed separately. It does not mix or retain base FL/FR in the seven
  processed inputs, and preserves VHL/VHR provenance for the top slots.
- **PASS, diagnostic native config-4 session entry:** J0A6 consumes the actual
  J0A4 qualification and J0A5 seven-slot/LFE owned vectors, builds the existing
  `eac3jocsession::Input` with `joc_num_channels=7/config=4`, and reuses the
  B1/B2A/B2B metadata path. The three-AU run emits finite 15-object/LFE
  Gate6C batches and one native 577-sample Gate6C flush batch; production
  acceptance remains
  `INCONCLUSIVE` and no renderer/playback path is entered. This is a probe-only
  integration result, not a production decoder claim.
  generic native E-AC-3/JOC, reusable production ownership, and production
  demux integration remain open.

- **PASS, diagnostic prepared-scene entry:** J0A7 exposes the actual J0A6
  Gate6C batches through a probe-local `BatchCallback`, retaining owned PCM
  vectors and the B1/B2A/B2B metadata. `Eac3NativeConfig4SceneProbe` adapts
  the 15 dynamic-object metadata records through the existing
  SceneAdapter metadata-to-gain-frame contract while keeping decoded object
  PCM and LFE PCM separate; SceneAdapter's LFE field is presence/gain metadata,
  not a substitute for the owned LFE vector. The native intervals remain
  exactly `959/1536/1536/577` at starts `0/959/2495/4031`, with no padding or
  timestamp translation. Gain-frame timestamps follow the actual metadata
  events at `0/1536/3072`, with the held flush frame queried at `4031`.
  Self-test passed 5 cases covering callback poison,
  reset, cancel, stale generation, and future metadata rejection. The raw
  five-second EB3 and all six five-second DEE files passed the three-AU
  matrix (`7/7`); each had 15 objects, separate LFE, and independent owned
  vectors. This is a prepared-scene diagnostic only: R2C2 now consumes its
  retained room positions through the sample-accurate Cartesian System H path,
  but no object PCM is sent to production playback and no DRC or production
  acceptance is claimed.

Evidence: `docs/dev/eac3-joc-next-roadmap.md:843-870` and
`docs/bug/media-foundation-status.md:325-352`; J0A4 probe target
`build-mm/Debug/Eac3NativeConfig4JocBridgeProbe.exe`; J0A5 target
`build-mm/Debug/Eac3NativeConfig4MappingProbe.exe`; J0A6 target
`build-mm/Debug/Eac3NativeConfig4JocSessionProbe.exe`.

### Scene and panner

- **PASS, bounded fixed layout:** R0A/R0B/R0C/R0D implement the fixed BS.2051
  System H point-source gain path, nominal-to-actual adaptation, lower
  VirtualNgon, and the five direct-downmix virtual speakers.
- **PASS, renderer-neutral seams:** R1A and R1B1-R1B5 cover explicit bed/LFE
  policy, OAMD coordinate conversion, target grouping, causal ramp scheduling,
  and a 22-speaker System H gain frame.
- **Open:** non-zero extent math, diffuse/direct split, decorrelation,
  divergence, and the remaining BS.2127 P1.1 properties. Non-zero extent is
  intentionally `EXTENT_PENDING`, not silently treated as a point source.

Evidence: `docs/dev/eac3-joc-next-roadmap.md:587-738` and
`docs/bug/media-foundation-status.md:7-111,148-303`.

### SOFA, BRIR, and output

- **PASS, bounded:** R2A1 validates the local BBC System H SOFA metadata and
  creates a native cache with explicit emitter/receiver mapping.
- **PASS, bounded:** R2B1 provides scalar partitioned CPU convolution and exact
  tail handling for the asset's all-zero delays.
- **PASS, diagnostic I0P scaffold/contract:** the real R2C2 22-speaker bus now
  feeds the existing R2B1 stream, writes little-endian IEEE float32 stereo WAV,
  a separate float32 mono LFE stem, and a structured JSON report. The bridge
  enforces generation, continuous sample time, 22-channel shape, finite PCM,
  exact source/EOS/convolution-tail accounting, and the all-zero-delay cache
  contract. It applies no normalization, DRC, limiter, or postgain.
- **PASS, diagnostic current-asset mixer:** R2C performs sample-accurate
  deterministic summation into a canonical 22-speaker planar bus. Cartesian
  routing now consumes retained J0A7 room positions through the probe-local
  fixed System H panner; the older UnitVector3/SceneAdapter path remains
  unchanged. Bed PCM is explicitly empty; LFE remains an independent
  `SeparateFromPointLayout` sideband. No normalization, DRC, limiter, postgain,
  BRIR, WASAPI, or production playback is applied. Broader spatial acceptance
  remains under review.
- **Open, parallel/deferred:** R2B2 fractional/non-zero delay support for
  broader assets; the current BBC asset has all-zero delays, so R2B2 does not
  block its I0P path. I0P is a probe-local offline sink, not an integrated
  production `BinauralRenderer` or player sink.

Evidence: `docs/dev/eac3-joc-next-roadmap.md:740-796`,
`docs/bug/media-foundation-status.md:113-146,2476-2498`, and
`docs/dev/eac3-joc-renderer-assets.md:26-101`.

R2C evidence (2026-08-29): `cmake --build build-mm --config Debug --target
Eac3NativeConfig4SceneProbe Eac3NativeConfig4R2CProbe --parallel 4` followed by
`Eac3NativeConfig4R2CProbe --self-test` originally passed 39 cases, including fixed
Cartesian System H center/radius/cardinal/mapping and a hard-coded interior
point oracle independently derived from the BS.2127-1 separable equations
(center is M+090/M-090 at sqrt(0.5), with cardinal points asserting
exact speaker identities), direct single/two-object and object-11 single-channel
sum oracles, silence, finite and exact object/bed/LFE policy rejection,
future-gain boundary, evaluator generation locking and stale/mixed generation,
callback poison, reset, cancel, max-AU zero/overflow rejection, and separate
LFE handling. The raw five-second EB3 and all six five-second DEE files each
passed the bounded three-AU matrix (`7/7`), yielding 22 finite planar channels
and an independent LFE sideband across exactly `4608` samples at
`959/1536/1536/577`. The pre-Cartesian bounded digest was
`0x22ed9b74eb62a383`; the current raw 157-AU Cartesian digest is
`0xf416ad980af44030`. The Cartesian future-point pass evaluates each real
sample; the `--max-aus 157` scan produced 157 AU plus EOS (158 batches,
`241152` samples) for the raw asset and all six DEE vectors without the prior
antipodal-crossing failure. Full-duration production spatial acceptance remains
INCONCLUSIVE pending independent renderer/oracle evidence. This is diagnostic
speaker-bus evidence, not I0 stereo, BRIR, or production acceptance.

Level instrumentation (2026-08-29) reports observations only, with no
normalization, threshold, or gain change. All seven three-AU prefixes are exact
silence on both the 22-speaker bus and LFE sideband. The raw 20-AU run is also
exactly silent on the speaker bus (`peak=0`, `RMS=0`) while LFE is finite but
very small (`peak=1.57436105e-07`, `RMS=2.15444656e-08`). The adjacent raw
21-AU run becomes non-zero after the first metadata-change/ramp boundary:
speaker bus `peak=1.37705883e-05`, `RMS=3.46381757e-07`; LFE
`peak=5.90061472e-06`, `RMS=5.31141416e-07`. This resolves the 20-AU zero as a
window boundary rather than proving a permanently silent mixer, but the very
small native PCM amplitude still lacks an external oracle and blocks I0
amplitude/listening acceptance.

I0P evidence (2026-08-29): the Release R2C probe self-test covered 40 cases;
the added case covers a synthetic impulse/marker, generation/timeline/shape/
nonfinite/empty rejection, exact float-WAV headers and data sizes, required
report fields, deterministic reset, and absence of a stale pre-reset tail.
The real command
`Eac3NativeConfig4R2CProbe --max-aus 21 --i0p-cache
tmp/r2a-system-h-brir.cache --i0p-stereo-wav
tmp/i0p/powder-snow-21au-stereo-f32.wav --i0p-lfe-wav
tmp/i0p/powder-snow-21au-lfe-f32.wav --i0p-report
tmp/i0p/powder-snow-21au-report.json "media/POWDER SNOW Live V9.8.6.eb3"`
passes with `32256` source/LFE frames, `16383` tail frames, and `48639`
stereo frames at 48 kHz. Stereo is finite and non-zero (`peak=9.6324793e-06`,
`RMS=5.52253921e-07`, digest `0xafbe657bf82ac242`); LFE is finite and non-zero
(`peak=5.90061472e-06`, `RMS=5.31141416e-07`, digest
`0xfa2dc1aaa802efce`). `ffprobe` identifies the stereo artifact as
`pcm_f32le`, 48 kHz, two channels, `duration_ts=48639`. PE `/DEPENDENTS`
contains only MSVC/UCRT and `KERNEL32.dll`, with no FFmpeg/libav import.
`scripts/validate-all.ps1` also passes all 10 unit-test suites, report-schema
self-test, and playback smoke; aggregate report is
`build-mm/validation-report.json`.
These are internal/offline PCM facts only: `amplitudeOracle=INCONCLUSIVE` and
`productionAcceptance=INCONCLUSIVE`; no audible-quality threshold or gain
compensation was introduced.

I0 amplitude/program audit (2026-08-29):
`scripts/run-atmos-i0-amplitude-oracle.ps1` uses the project's self-built
`runtime-with-ffprobe-msvc` tools, disables FFmpeg DRC/target-level scaling,
and decodes exactly the same first 21 audio frames (`32256` PCM frames). The
external 5.1.2 result is already very small (`peak=4.30027831e-05`,
`RMS=1.57096044e-06`), while native J0A6/J0A7 object PCM has
`peak=1.37705883e-05`, `RMS=4.19488897e-07`; R2C has the same peak and
`RMS=3.46381757e-07`, with `evaluatedGainPeak=1`. Together with the normative
factor-of-two overlap/add already implemented from TS 102 366 clause 6.9.4,
this is a **PASS only for an amplitude-scale reference**: there is no evidence
for a missing large fixed transform gain, so no decoder gain correction was
made.

Program completeness remains **BLOCKED/INCONCLUSIVE**. FFmpeg's
backward-compatible 5.1.2 FBW output is non-zero from frame 0, whereas native
JOC objects and the R2C speaker bus first become non-zero at sample `30144`;
LFE agrees much more closely (`28416` in both paths). TS 103 420 defines JOC as
a post-processor that reconstructs output objects from the configured downmix,
and config 4 intentionally uses dependent phase-shifted L/R instead of base
FL/FR, so this difference does not by itself prove that a bed was dropped.
However, the project has no independent object-reconstruction/energy oracle
proving that those 15 objects preserve the complete non-LFE programme. The
current `bed=EMPTY` R2C result therefore cannot pass I0 listening acceptance.
The next gate is an independent JOC programme-completeness oracle (or a
standards-derived downmix/reconstruction consistency check) before any bed
ownership change, gain compensation, or listening preview.

J0A6 layer audit narrows the zero prefix further. Across the same 21 AUs,
the seven mapped downmix inputs are non-zero from sample `0`
(`peak=4.21920704e-05`, `RMS=1.46721997e-06`), but the interpolated JOC
reconstruction matrix is exactly zero until QMF timeslot start `30720`.
Reconstructed object QMF is also exactly zero until `30720`, satisfying the
TS 103 420 clause 6.6.6 forward equation for the complete zero-matrix prefix
(`forwardMatrixConsistency=PASS_ZERO_PREFIX`). QMF synthesis/filter delay then
places the first non-zero object PCM at `30144`; J0A7 and R2C preserve that
same boundary. This rules out SceneAdapter, Cartesian panning, BRIR, and a
fixed gain as causes of the `30144` boundary.

This forward consistency result still cannot establish programme
completeness. TS 103 420 specifies `Qout = matrix * Qin`, but does not provide
an inverse object-to-downmix matrix, a renderer/downmix energy-conservation
identity, or a reference decoded object signal for this bitstream. Therefore a
standards-only inverse oracle cannot be honestly constructed from the
available inputs. The minimum remaining dependency is either an independent
JOC decoder exposing the same 15 object signals, or trusted reference object
stems plus their authoring/downmix matrix. Until one exists, the zero prefix is
explained by coded matrix state but complete non-LFE programme preservation
remains `BLOCKED_NO_NORMATIVE_INVERSE_DOWNMIX`.
Evidence command: `scripts\run-atmos-i0-amplitude-oracle.ps1 -MaxAUs 21
-BuildDir build-mm -Configuration Release`; report:
`tmp/i0-oracle/amplitude-oracle-21-au.json`. Focused J0A6 and R2C self-tests
are `14/14` and `40/40`;
`scripts\validate-all.ps1 -BuildDir build-mm -Configuration Debug` passes all
10 test suites, report schema, and playback smoke, with aggregate report
`build-mm/validation-report.json`.

I0 internal listening artifact (2026-08-29): TS 103 420 defines the configured
downmix as input to reconstruction of the JOC output object signals; it does
not identify those output objects as residuals that must be added back to the
compatibility downmix. The safest non-duplicating internal policy is therefore
`JOC_RECONSTRUCTED_OBJECTS_ONLY_NO_ADDITIVE_DOWNMIX`: render the 15 J0A6 output
objects through the existing BS.2127 System H and BRIR chain, keep LFE as the
existing separate diagnostic sideband, and do not also mix base FL/FR or the
seven mapped downmix channels. This policy permits internal listening while
external decoder equivalence remains `INCONCLUSIVE_NO_INDEPENDENT_OBJECT_REFERENCE`.

`scripts/render-atmos-i0-listening-preview.ps1` renders one 157-AU natural-level
stereo artifact plus the required separate LFE diagnostic sidecar and reports
the policy explicitly. The supplied raw EB3 produced `241152` source frames
(`5.024` s) plus the exact `16383`-frame BRIR tail, for `257535` stereo frames
(`5.3653125` s) at 48 kHz. Stereo is float32, finite and non-clipping with
`peak=0.1007793546` (`-19.93` dBFS) and `RMS=0.01166294318`
(`-38.66` dBFS). No monitor gain, normalization, DRC, limiter, or postgain was
used. This is `PASS_INTERNAL_LISTENING_ARTIFACT`, not proof of Dolby/reference
renderer equivalence; subjective listening remains a manual observation.

Config-3/M4A offline entry (2026-08-30): `media/03. iPad.m4a` contains one
E-AC-3 audio stream at index `0` (48 kHz, six channels, `5.1(side)`, 768 kb/s,
container duration `202.462333` s). Native JOC inspection reports downmix
config `3`, five JOC input channels, and 15 output objects. The existing J0A3
ordinary-core/JOC bridge is now admitted to the same J0A7 prepared-scene and
R2C/I0P path through an explicit `--joc-config 3`; config 4 remains unchanged.
A real three-AU config-3 integration run passes R2C and I0P with 15 objects,
separate LFE, finite stereo, and no postgain.

The full-file run packet-copied `6327` complete AUs (`19436544` sidecar bytes)
and rendered `9718272` source frames (`202.464` s) plus the exact
`16383`-frame BRIR tail, for `9734655` stereo frames (`202.8053125` s).
Natural float32 stereo is finite but exceeds unity (`peak=1.3550338745`,
`RMS=0.2108538095`), so it is retained as evidence rather than presented as
the safe listening file. A separately named post-BRIR monitor copy applies
only `0.7010894841` linear (`-3.08453094` dB), yielding `peak=0.95` and
`RMS=0.1478273885`; this is explicitly not a decoder scaling correction,
normalization, DRC, or limiter. The measured full offline run took about
19 minutes and peaked near 2 GB working set on this machine.

I0 offline performance pass (2026-08-30): profiling the real config-3 iPad
route at 157 AUs separated packet copy (`0.083` s), native decode/JOC/QMF
(`3.71` s), Cartesian per-sample gain evaluation plus System H mixing
(`23.47` s), metrics (`0.02` s), and BRIR/WAV (about `1` s). The hotspot was
therefore the renderer's independent per-sample Cartesian evaluations, not
BRIR convolution. `--jobs N` now evaluates disjoint samples concurrently;
each sample retains the same object/speaker order and floating-point sums.
The listening wrapper exposes `-Jobs N`, with `0` selecting up to eight jobs.

The 157-AU BRIR/WAV A/B was byte-equivalent: Jobs 1 took `28.149` s at
`64.09` MiB, Jobs 8 took `9.861` s at `65.09` MiB, and both produced speaker
digest `0x2225f4a89152410c` and stereo digest `0xc7ca79be942c3608` with
identical frames, tail, peak, and RMS. The optimized full `6327`-AU iPad run
took `419.85` s wrapper wall time (`407.38` s native) and peaked at
`1089.99` MiB native / `1199.29` MiB combined. Its natural stereo WAV SHA-256
`0BC03A038052A20C3D87CF19810BC13F3ED58DF946D8732963737E82DC126CAA`,
speaker/stereo digests, frames, peak, and RMS are byte-identical to the prior
single-job full result. The mixer now streams each completed 22-channel batch
directly into metrics/BRIR instead of retaining the complete speaker bus, but
prepared object batches are still retained for future-point metadata
evaluation; memory remains linear and true bounded-memory long-track
rendering is a later gate.

The listening wrapper now accepts raw `.eb3`/`.ec3`/`.eac3` directly and M4A
through a selected E-AC-3 stream. M4A extraction uses access-unit packet copy
to a raw sidecar (`codecCopy=YES`, `transcode=NO`), never audio decode/re-encode.
`-FullFile` counts complete AUs to EOF and output names derive from the input.
This is still diagnostic: M4A priming/end trim is reported but not applied,
and R2C retains full prepared object batches in memory, so long-track cost is
still linear despite streaming the mixed speaker bus. It does not claim I2
native container/trim/seek completion.

MONTERO listening A/B (2026-08-30): the Atmos and ALAC files share title,
artist, album, ISRC `USSM12100531`, UPC `886449511440`, and creation metadata,
so they are the same release identity, but this does not prove the same master.
Their container starts are both zero and durations differ by `64.005` ms. A
100-second 8-kHz mono diagnostic correlation found the Atmos mix delayed by
`4.75` ms (`r=0.485`); the ALAC reference crops use that offset and retain
44.1-kHz stereo content. The native config-3 path decoded all `4306` AUs from
the head before the final binaural WAV was cropped, preserving decoder/JOC/QMF
state. The full render used eight jobs and the existing non-additive
reconstructed-object policy; its safe listening source has one explicit
post-BRIR `-7.018324` dB monitor attenuation (`peak=0.95`), not a decoder gain.

The requested A/B windows are `75-90` s, `114-125` s, and `50-60` s. Each
ALAC reference uses one reversible RMS-match gain (`-10.417232`, `-10.366778`,
and `-11.216664` dB respectively); no limiter, normalization, remix, or sample-
rate conversion was applied. Six float32 WAVs plus `manifest.json` and
`report.md` are under `tmp/montero-ab`. Post-BRIR 100-ms interaural activity
and the separate LFE stem were measured per window, but these are only
listening/position proxies, not an independent object-coordinate oracle.
External Dolby/JOC renderer equivalence remains inconclusive. Focused Release
self-tests pass `45/45` scene cases and `41/41` R2C cases;
`scripts\validate-all.ps1 -BuildDir build-mm -Configuration Debug` passes all
10 unit-test suites, report schema, and playback smoke, with aggregate report
`build-mm/validation-report.json`.

Object-property contract pass (2026-08-30): the former listener-centre
workaround has been removed. OAMD scene updates now carry an explicit
`EtsiRoomCartesian` type through `SceneAdapter`, Cartesian ramp/hold/reset,
and the BS.2127 allocentric panner. `(0.5,0.5,0.0)` is evaluated as the real
allocentric centre `(0,0,0)`; no fabricated unit direction is stored or used.
The generic directional path still requires a finite unit vector, while screen
anchor and infinite distance remain structured unsupported until their
geometry contracts exist.

`Eac3NativeConfig4SceneProbe --joc-config 3 --max-aus 3907` decoded MONTERO
from the stream head through 125 s in `105.84` s without R2C/BRIR. Windows
A/B/C contained `7035/5160/4680` object-property updates; every update had
non-zero priority, and B/C each had two listener-centre updates. Non-zero size,
restricted zone, snap, screen anchor, and distance counts were zero in all
three windows. Priority is now finite/range-checked and retained in scene gain
frames, but deliberately does not affect gain because no cited BS.2127 rule
maps priority to amplitude. The full 21-row coverage matrix and evidence paths
are machine-checked by `scripts/test-atmos-property-coverage.ps1`; report:
`tmp/montero-ab/property-audit-125s.txt`.

### R2C2 — Cartesian future-point evaluated gains (diagnostic PASS, 2026-08-29)

R2C2 adds a probe-local offline future-point pass: it collects the actual J0A7
owned batches and retained room-position points through the probe-local fixed
System H Cartesian panner, evaluates gains at each real PCM sample, and supplies the
resulting `[sample][object][speaker]` gains to the mixer one PCM batch at a
time. The focused R2C self-test now passes 41 cases, including Cartesian center,
radius, cardinal, mapping, a hard-coded interior-point BS.2127 equation oracle,
per-sample gain, and serial/8-job evaluated-gain byte equivalence
oracle, explicit ramp-duration gap/overlap/step/jump semantics, and staged
gain-count overflow rejection; the default
three-AU raw run remains a finite deterministic PASS with
`evaluatedGainFrames=PASS` and `evaluatedInterpolation=YES`.

The prior exact antipodal crossing at sample `28416` is now evaluated as the
allocentric Cartesian center by the fixed BS.2127 pseudocode; no object
position is normalized, slerped, held, or epsilon-shifted. Raw
`--max-aus 157` (also reproduced with `--max-aus 20`) produces finite
`241152` samples and one EOS tail, with `evaluatedGainFrames=PASS` and the
real gain change at sample `29184`. This is diagnostic spatial-gain evidence;
comparison against an independent renderer implementation, PCM amplitude
validation, and production renderer acceptance remain `INCONCLUSIVE`.

### J0A8 — max-AU and final-EOS ownership (2026-08-29)

J0A6, J0A5, J0A7, and R2C now accept an explicit `--max-aus N` (default `3`)
where applicable, check zero/overflow before deriving `N*1536`, and derive
the final decoder/mapping/scene/EOS interval from the requested AU count rather
than ordinal `2`. The bounded three-AU behavior remains unchanged. For the
seven supplied five-second EB3 files, `--max-aus 157` produced exactly 157
decoded AU and one 577-sample EOS batch (`241152` total samples, 158 batches,
nominal `5.024 s` at 48 kHz) with continuous starts; the raw and all six DEE
runs were finite and the Cartesian future-point pass completed each scan.
Thus J0A8 proves full-length scan/cursor ownership and exact EOS accounting,
while spatial output remains diagnostic and production acceptance remains
INCONCLUSIVE.

### Integration and production

- **I0P diagnostic PASS:** the all-zero-delay BBC cache now has an offline
  R2C-to-R2B1 bridge with exact decoder/EOS-tail and convolution-tail
  accounting, finite level observations, deterministic reset, float32 WAV/
  JSON output, and a separate LFE stem. The policy remains explicitly
  `ExcludedFromBinaural+SeparateStem`; it is reversible and is not the final
  listening-quality LFE decision. I0 still cannot pass as first-audible until
  the very small native PCM amplitude is compared with an external PCM/
  reference oracle and the artifact receives an evidence-bounded listening
  check.

- **Diagnostic PASS:** Gate 7A/7B/7C/7D cover synthetic endpoint lifecycle,
  property conversion, bounded queue/consumer operation, real object
  submission, synchronized loopback, and basic subjective acceptance. This is
  endpoint-submission/endpoint-output evidence, not Dolby Atmos for Headphones
  equivalence or formal self-rendered binaural evidence.
- **Not accepted:** I0 now has a diagnostic raw-EB3 stereo/LFE/report artifact
  from the I0P scaffold, but first-audible amplitude/listening acceptance is
  still `INCONCLUSIVE`. I1 ordinary stereo WASAPI lifecycle, I2 native M4A
  packet/trim/seek, and I3 production routing/release validation remain not
  started.

Evidence: `docs/dev/eac3-joc-next-roadmap.md:819-912`,
`docs/dev/eac3-joc-production-playback.md:64-109,111-197`, and
`docs/bug/media-foundation-status.md:1567-1644`.

## Current implementation slice

N5C is now a probe-local/native diagnostic acceptance seam for the supplied raw
config-4 topology: it pairs the Legacy AC-3 base plus its immediately
following dependent E-AC-3 substream into stable channel identities, but it
does not yet provide native PCM or a reusable production API. N5D now accepts
the supplied Legacy AC-3 BSI/audblk syntax through a complete six-block
coefficient cursor boundary. N5E now materializes finite renderer-neutral
coefficient values with exact vector sizes and deterministic digests. N5F now
connects the values to the existing Legacy AC-3 IMDCT/overlap path for a
diagnostic six-block base-frame PCM result, but not dependent assembly. N5G now
accepts the supplied dependent SID0 audblk/coefficient/transform path as four
stable diagnostic PCM channels, but it still does not combine those samples
with the base frame. N5H provides first-AU dependent/base PCM assembly and N5I
now provides a probe-local stateful three-AU owner; preserve the six-block/
1,536-sample AU contract and explicit LFE policy. The stateful seam retains
dither and IMDCT/overlap state, while parser syntax state remains frame-local.
Do not infer support from the E-AC-3 profile flag, and do not route this slice
into production playback. J0A5 now applies the normative config-4 seven-slot
mapping to actual N5J vectors, and J0A6 enters the shared renderer-neutral
native JOC session with those vectors. R2C's current-asset speaker-bus mix is
now available; the next slice is I0. Keep R2B2 as a parallel/deferred slice
for non-zero/fractional-delay assets, before broader mature acceptance.

N5H provides the first-AU ten-channel base/dependent diagnostic assembly
through linked probe-local diagnostic contracts with explicit source prefixes
and separate EOS tails. N5I retains dither and IMDCT/overlap state across
three adjacent diagnostic AUs, and N5J explicitly owns each frame boundary
while rebuilding frame-local parser syntax. J0A4 now proves config-4
base/dependent EMDF and JOC qualification association without consuming,
transforming, dropping, or mixing the N5J PCM vectors. J0A5 consumes the owned
vectors to construct the diagnostic mapped-session input contract and retains
explicit provenance. J0A6 consumes both contracts at the shared native session
seam; the syntax/oracle gates and production acceptance remain required.

J0A4 evidence (2026-08-28): `Eac3NativeConfig4JocBridgeProbe` focused build
passed. Its self-test passed `cases=8`, covering callback rejection/poisoned
flush, base-none/dependent-one carrier evidence, missing/duplicate dependent
container rejection, accepted preflight with explicit blocked qualification,
reset, and cancel. The supplied raw config-4 plus six DEE vectors each passed
three AU associations: carrier evidence `BASE_NONE_DEPENDENT_ONE`, payload
carrier `dependent/sid0`, payload11 `67` bytes, payload14 `216` bytes, and JOC
`config=4`, `channels=7`. The bridge reported
`sessionInput=NOT_ENTERED` and
`bridgeResult=BLOCKED reason=CONFIG4_JOC_BRIDGE_BLOCKED_SESSION_CORE_CONFIG3_ONLY`.
This is association/qualification evidence only; no config-4 PCM-to-session
mapping, renderer, production playback, or FFmpeg runtime path is claimed.

J0A5 evidence (2026-08-28): `Eac3NativeConfig4MappingProbe` links the
probe-local J0A4 qualification contract and N5J PCM contract without including
any `.cpp` implementation; each mapped AU is joined by actual qualification
ordinal/timestamp/carrier fields. Its
deterministic self-test passed `cases=16`, covering slot/provenance ordering,
config/chanmap/count/order/timeline/tail/finite rejection, and owned-vector
no-alias mutation. The raw config-4 plus six DEE matrix passed three AUs per
file: mapped starts `0/1536/3072`, seven slots with `1536` samples each, no
intermediate tails, and one final `256`-sample tail for every slot plus the
separate `base.LFE` bypass. The mapping follows ETSI TS 103 420 V1.2.1
Tables 47, 48, and 53, and TS 102 366 V1.4.1 Table E.1.4 and clause E.2.8.2
for config-4 slots and dependent replacement/supplement semantics. It remains
diagnostic-only: `sessionInput=NOT_ENTERED` and production acceptance is
`INCONCLUSIVE`.

Exact evidence commands: `cmake --build build-mm --config Debug --target
Eac3NativeConfig4MappingProbe -- /m:4`; `build-mm\\Debug\\Eac3NativeConfig4MappingProbe.exe
--self-test`; the raw-plus-six-DEE `--max-aus 3` matrix; and
`dumpbin.exe /DEPENDENTS build-mm\\Debug\\Eac3NativeConfig4MappingProbe.exe`.
The final `scripts\\validate-all.ps1 -BuildDir build-mm -Configuration Debug`
passed unit tests, report schema, and smoke; aggregate report:
`build-mm\\validation-report.json`.

J0A6 evidence (2026-08-28): `Eac3NativeConfig4JocSessionProbe` links the
probe-local J0A4/J0A5 contracts and enters the existing native
`eac3jocsession::Session` with the actual qualified config-4 seven-slot input.
The deterministic session/owner self-test passed `cases=12`; the real raw
config-4 run passed three AU callbacks at native output intervals
`959/1536/1536` samples and one native Gate6C flush batch of `577` samples;
the emitted total is exactly `4608` samples. No zero padding or timestamp
translation is applied. The N5J per-AU `256`-sample decoder tails are shape
checked but are not consumed by the session, so decoder-tail integration and
production trim remain `BLOCKED`/`INCONCLUSIVE`. Metadata was produced through
B1/B2A/B2B, and the input remained finite and config4/7-qualified. This is
diagnostic-only session entry; no renderer, production playback, DRC, or
FFmpeg runtime path was exercised.

Exact J0A6 evidence commands: `cmake -S . -B build-mm`; `cmake --build
build-mm --config Debug --target Eac3NativeConfig4JocSessionProbe
Eac3NativeConfig4MappingProbe Eac3NativeConfig4JocBridgeProbe
Eac3NativeConfig4PcmAssemblyProbe Eac3JocSessionProbe -- /m:4`;
`build-mm\\Debug\\Eac3NativeConfig4JocSessionProbe.exe --self-test`; and
the raw-plus-six-DEE three-AU matrix. The focused target reported PE imports
only `MSVCP140D.dll`, `VCRUNTIME140D.dll`, `VCRUNTIME140_1D.dll`,
`ucrtbased.dll`, and `KERNEL32.dll`; `scripts\\validate-all.ps1
-BuildDir build-mm -Configuration Debug` passed with aggregate report
`build-mm\\validation-report.json`.

N5C evidence (2026-08-28): `cmake -S . -B build-mm` and
`cmake --build build-mm --config Debug --target Eac3NativeConfig4CoreProbe --
/m:4` passed. `Eac3NativeConfig4CoreProbe.exe --self-test` passed 10 cases.
The supplied raw file `K:\Qt\AudioPlayer\media\POWDER SNOW Live V9.8.6.eb3`
passed three units; each of the six
`K:\Qt\AudioPlayer\media\gate8-dee-vectors\eb3\*.eb3` files passed its first
unit. Results were `channelIdentity=PASS`, `sampleTimeline=PASS`,
`callbackBoundary=PASS`, `pcmAvailability=UNAVAILABLE_LEGACY_AC3`,
`drcApplied=NO`, `ffmpegLinked=NO`, and
`probeResult=PASS stage=gate8n-5c-config4-core-substream`.
The PE `/DEPENDENTS` audit of
`build-mm\Debug\Eac3NativeConfig4CoreProbe.exe` passed with only MSVC/UCRT and
`KERNEL32.dll` dependencies. `scripts\\run-tests.ps1` passed all 10 suites;
`scripts\\validate-all.ps1` passed unit tests, report schema, and smoke. The
full validation report is `build-mm\\validation-report.json`.

N5D evidence (2026-08-28): `cmake --build build-mm --config Debug --target
Eac3NativeLegacyAc3FrontendProbe -- /m:4` passed. The focused probe
`--self-test` passed 11 cases, including staged exponent-field order, actual
interleaved SNR/gain fields, and one shared grouped-mantissa cursor across
FBW channels/LFE. The raw config-4 frame passed six-block coefficient cursor
accounting through bit 20456; the six DEE first-base checks passed with the
1152k vector ending at bit 18413 and the other five at bit 20456. The result
is `COEFFICIENT_ONLY` / `PCM_UNAVAILABLE_LEGACY_AC3`; no coefficient values,
IMDCT, overlap, or production PCM were claimed. `scripts\\validate-all.ps1`
passed unit tests, report schema, and smoke; report:
`build-mm\\validation-report.json`. PE `/DEPENDENTS` reported only
MSVC/UCRT and `KERNEL32.dll` dependencies, with no FFmpeg/libav imports.

N5E evidence (2026-08-28): `cmake --build build-mm --config Debug --target
Eac3NativeLegacyAc3FrontendProbe -- /m:4` passed after linking the existing
`native-eac3-block-state.cpp` digest helper. The focused probe
`--self-test` passed 12 cases, including grouped BAP cursor continuation,
dither on/off, finite/range/vector-size checks, and reset determinism. The raw
config-4 first frame passed with `coefficientCount=6552`,
`stateDigest=0xbfdcb1c364043e41`, bit 20456, and each FBW/LFE boundary emitted
217/7 finite values. The six DEE first-base checks all passed with the same
6,552-value count; `powder_active_5s_1152k.eb3` ended at bit 18413 with
`stateDigest=0xefd62a312d1186b3`, while the 1280k/1408k/1512k/1536k/1664k
vectors ended at bit 20456 with `stateDigest=0xbfdcb1c364043e41`. The
per-channel coefficient digests were deterministic across the matrix; no
checked-in spectral oracle was available, so no FFmpeg comparison was claimed.
All results remain `COEFFICIENT_ONLY` / `PCM_UNAVAILABLE_LEGACY_AC3`.
`scripts\\validate-all.ps1` passed unit tests, report schema, and smoke; report:
`build-mm\\validation-report.json`. PE `/DEPENDENTS` remained limited to
MSVC/UCRT and `KERNEL32.dll`, with no FFmpeg/libav imports.

N5G evidence (2026-08-28): `cmake --build build-mm --config Debug --target
Eac3NativeDependentPcmProbe -j 4` passed, and
`Eac3NativeDependentPcmProbe.exe --self-test` passed 16 cases: 9 direct
topology cases (one valid baseline plus wrong SID/type, acmod, LFE, channel
count, chanmap, missing chanmap, and weight), 1 independent channel-storage
case, and 6 lifecycle cases covering callback delivery, missing/rejected
callback backpressure, cancel, flush and post-flush rejection, poison,
reset/reopen, and stable channel identity. The
supplied raw config-4 first AU and the six DEE first-AU vectors
(`powder_active_5s_1152k.eb3`, `1280k`, `1408k`, `1512k`, `1536k`, and
`1664k`) all passed the strict Legacy AC-3 SID0 plus dependent E-AC-3 SID0
topology gate (`48 kHz`, six blocks, dependent `acmod=5`,
`chanmap=0xA010`, weight `4`). Each dependent channel `L/R/VHL/VHR` reported
`sampleCount=1536` and `eosTailCount=256`, all finite, with
`baseSampleStart=dependentSampleStart=0` and first-AU ordinal
`sampleTimeline=PASS`; this is not a multi-AU timestamp claim. The raw
dependent state digest was `0xDED5BDFA89C121BA`; the six DEE dependent state
digests were respectively `0xEEE79953BDFEA8F9`, `0x494DBA36879F4703`,
`0x7678A74FD7049CA6`, `0xF3716C3CF804541F`, `0x325BA809A603A220`, and
`0xDED5BDFA89C121BA`. All four channels in the raw and five non-1152k DEE
outputs had PCM digest `0x63036768322D2DD2`, peak `6.48314022332e-07`, RMS
`1.763614939e-07`, EOS digest `0x8A216254E26FE5F8`, EOS peak
`4.75796975986e-07`, and EOS RMS `1.44452961618e-07`; the 1152k vector's
dependent PCM digest was `0xF0D86198DC2D6542`, peak `7.81046343591e-07`, RMS
`2.4014778909e-07`, EOS digest `0x24B4088EEC836D19`, EOS peak
`6.36850302843e-07`, and EOS RMS `2.06355333118e-07`. The probe reports
`PCM_AVAILABLE_DIAGNOSTIC_DEPENDENT_AC3` but production acceptance remains
`INCONCLUSIVE`: no checked-in dependent/base spectral or PCM oracle exists,
and no assembly, JOC, renderer, DRC, or playback path was exercised.
`scripts\\validate-all.ps1` passed unit tests, report schema, and smoke; report:
`build-mm\\validation-report.json`. PE `/DEPENDENTS` reported only
MSVC/UCRT and `KERNEL32.dll`, with no FFmpeg/libav imports.

N5H baseline evidence (2026-08-28, before N5I state ownership):
`cmake --build build-mm --config Debug --target
Eac3NativeConfig4PcmAssemblyProbe -j 4` passed. The assembly probe self-test
passed 9 cases covering ten-channel ordering, unique source-prefixed
identities, no-alias synthetic storage, channel-count mismatch, separate AU
and EOS-tail shape, and open/push/callback rejection/cancel/flush/reset
lifecycle behavior. The raw config-4 run observed three valid base/dependent
AUs and assembled the first AU with
`multiAuContinuity=MULTI_AU_CONTINUITY_UNAVAILABLE`; all ten channels reported
`sampleCount=1536`, `eosTailCount=256`, finite peak/RMS, and
`baseSampleStart=dependentSampleStart=0` with first-AU ordinal timeline PASS.
The ten-channel order was exactly `base.FL, base.FC, base.FR, base.SL,
base.SR, base.LFE, dependent.L, dependent.R, dependent.VHL, dependent.VHR`.
The raw assembly digest was `0x4940c994e8c521f9`; base FBW/LFE retained the
N5F digests `0xf0d86198dc2d6542`/`0xa9868d057b544305`, while dependent
channels retained the N5G digest `0x63036768322d2dd2` (the 1152k DEE first AU
retained its distinct `0xf0d86198dc2d6542` dependent digest). Each of the six
DEE first-AU runs also observed three valid AUs and passed the same ten-channel
assembly gate; the 1152k assembly digest was `0x802919ebcacbd63d` and the
other five were `0x4940c994e8c521f9`. Assembly counts/digests/peak/RMS were
recomputed from the owned vectors rather than copied from source summaries. The result is
`PCM_AVAILABLE_DIAGNOSTIC_CONFIG4_ASSEMBLY` but production acceptance remains
`INCONCLUSIVE`: no base/dependent oracle exists and no multi-AU stateful
continuity, JOC, renderer, DRC, or playback path was exercised. PE
`/DEPENDENTS` reported only MSVC/UCRT and `KERNEL32.dll`, with no FFmpeg/libav
imports. `scripts\\validate-all.ps1` passed unit tests, report schema, and
smoke; report: `build-mm\\validation-report.json`.

N5I evidence (2026-08-28): `cmake --build build-mm --config Debug --target
Eac3NativeConfig4PcmAssemblyProbe Eac3NativeLegacyAc3FrontendProbe
Eac3NativeDependentPcmProbe -j 4` passed. The three focused self-tests passed:
N5F `cases=19`, N5G `cases=16`, and N5H `cases=14` with
`statefulOwner=PASS ownerLifecycle=PASS`. The raw config-4 fixture and all six DEE vectors each
reported `observedAUs=3 assembledAUs=3 statefulContinuity=PASS`; all three AUs
reported ten channels with `samplesPerChannel=1536`, sample starts `0`,
`1536`, and `3072`, and `eosTailPerChannel=0`, `0`, and `256` respectively.
The raw assembly digests were `0x0768bfd8e768209f`, `0x47c213c653020303`, and
`0x591831703e6da099`; the 1152k DEE digests were
`0x4439ef61e39de8f3`, `0x082b61aee8ce5c97`, and `0x65b693ba2f8d1489`, while
the other five DEE vectors matched the raw three-AU sequence. This is a
probe-local stateful dither/IMDCT-overlap result; parser syntax state remains
frame-local, no multi-AU oracle exists, and production acceptance remains
`INCONCLUSIVE`. The final frame uses the explicit EOS path before the owner is
flushed/closed; no JOC, renderer, DRC, or playback path was exercised.

N5J evidence (2026-08-28): the focused assembly target was rebuilt and its
self-test passed `cases=17` with `syntaxBoundary=PASS`, `abortReset=PASS`, and
`earlyParsePoison=PASS`; this includes explicit begin/complete ordering,
active-boundary rejection, abort poisoning until reset, and a real malformed
base parse through `StatefulAssemblyOwner` whose follow-up push/flush are
rejected until reset. The raw config-4 run passed three callbacks with
`syntaxOwnership=FRAME_LOCAL_RESET`, starts `0/1536/3072`, ten channels per
AU, and EOS tail counts `0/0/256`. The six DEE first-AU files were not changed
by this seam; a repeat raw-plus-six-DEE matrix after this hardening also
reported `observedAUs=3`, `statefulContinuity=PASS`, and
`syntaxOwnership=FRAME_LOCAL_RESET` for all seven files. The N5I seven-file
matrix remains the real-sample evidence for the stateful PCM owner. This closes
the syntax-state-open wording only; no cross-frame syntax carry is claimed.

N5F evidence (2026-08-28): `cmake --build build-mm --config Debug --target
Eac3NativeLegacyAc3FrontendProbe -- /m:4` passed after linking the existing
`native-eac3-transform.cpp`. The focused probe `--self-test` passed 19 cases,
including all-zero, impulse, single-bin, long/short, channel-isolation,
reset, and one-shot EOS checks through the existing transform contract. The
raw config-4 first frame and all six DEE first-base checks passed the
diagnostic PCM seam: each coded channel reported `sampleCount=1536` and
`eosTailCount=256`, all values finite. FBW channel 0 (identical for channels
1-4 in this supplied first frame) reported PCM digest
`0xf0d86198dc2d6542`, peak `7.81046343591e-07`, RMS `2.4014778909e-07`, EOS
digest `0x24b4088eec836d19`, EOS peak `6.36850302843e-07`, and EOS RMS
`2.06355333118e-07`; LFE channel 5 reported zero PCM with digest
`0xa9868d057b544305` and EOS digest `0x96127a562e3e7f08`. The five higher-rate
DEE vectors ended at bit 20456 with state digest `0x4f7269d8da57a95e`; the
1152k vector ended at bit 18413 with `0x029f978d932d8c5c`. These are finite
single-base-frame diagnostic values only; no offline PCM oracle was available,
so production acceptance is `INCONCLUSIVE`. `scripts\\validate-all.ps1`
passed unit tests, report schema, and smoke; report:
`build-mm\\validation-report.json`. PE `/DEPENDENTS` remained limited to
MSVC/UCRT and `KERNEL32.dll`, with no FFmpeg/libav imports.

The acceptance boundary remains 48 kHz, finite deterministic output, exact AU
and sample accounting, reset/flush correctness, and structured fail-closed
results for unsupported syntax. DRC application, hidden limiter/post-gain,
CUDA/SIMD, and production player changes remain out of scope for this slice.

## Evidence boundary and risks

The evidence ladder is syntax -> materialized coefficients/native PCM -> JOC/OAMD object
PCM -> BS.2127 gains -> SOFA/BRIR stereo PCM -> WASAPI submission -> endpoint
capture/listening. A lower layer cannot prove a later layer. The current
samples do not contain Annex-H headphone payload `0x7`; the Windows object
bridge therefore cannot be treated as a standardized headphone renderer.
The local SOFA asset is an ignored research cache with its own license and is
not a production asset decision.

2026-08-30 scene-contract closure: the decoder-to-scene boundary now carries
the effective TS 103 420 priority, three-axis extent and presence/index,
zone/elevation constraints, snap/channel-lock flag, screen anchor, distance
flags/factors, and explicit unsupported-property bits through
`ObjectMetadataUpdate`, ramp snapshots, and `ObjectGainFrame`. These fields
remain renderer-neutral: the BS.2127 point-source gain path does not apply
priority, extent, zone, snap, distance, or screen rules. Non-zero extent,
screen anchoring, and infinite distance remain explicitly unsupported/fail
closed until a normative renderer policy and required geometry are available;
no sound rule was invented. Focused probes and the 21-row property coverage
matrix passed. The supplied material still has no TS 102 366 Annex-H `0x7`
headphone payload, so no standardized BRIR payload claim is made.

2026-08-30 speaker/zone policy slice: the renderer-neutral scene adapter now
supports explicit System H speaker anchors (unmapped labels reject),
deterministic nearest-speaker snap/channel-lock with a 0.4 unit-vector
maximum distance, and zone candidate filtering with energy renormalization;
an empty eligible set fails closed. These are bounded TS 103 420 candidate
selection behaviors and do not claim a complete proprietary binaural policy.
The current E-AC-3/JOC decoder does not expose 5.5.12/5.6 warp/trim position
adjustment or balance state, nor TS 102 366 E.1.3.1.10/11 `lfemixlevcode`,
output mode, and LFE-output-disabled ownership. Therefore no warp/balance or
LFE-to-L/R mix is applied; LFE remains separate (`NO_MIX`) pending those
fields and programme ownership evidence.

Current risks are lack of an independent JOC object/programme-completeness
oracle and the explicitly frame-local parser syntax seam, the N3B production
PCM acceptance gap, extent/diffuse policy, the undecided final LFE listening
policy, deferred
R2B2 non-zero/fractional-delay coverage, and the absence of a native M4A
demux/seek-preroll rule. Existing ordinary
PCM, ASIO, ALSA, and static-bed behavior must remain unchanged until I0 is
accepted.

2026-08-30 third property audit: the existing MONTERO 125 s three-window
`tmp/montero-ab/property-audit-125s.txt` run reports 7035/5160/4680 updates;
divergence, warp, and trim fields were not emitted by the B1/B2A/B2B path.
The scene contract now accepts typed divergence/trim/warp values and applies
only the normative warp Yx2 transform; no divergence split or trim/balance
sound algorithm is claimed. Current payload extraction exposes B1/B2A/B2B
and opaque additional-data bytes, but no 5.5.12/5.5.13/5.5.14/5.5.15 wire
parser; this remains the next decoder gate, not a renderer wrapper.

## Status refresh: 2026-08-31 (BEAR metadata carriage and adapter audit)

The bounded BEAR bundle metadata contract now identifies itself as
'eac3-oamd-renderer-neutral', version 1, while retaining the binary 'BSCN'
version-2 PCM/LFE layout unchanged. 'tools/atmos-joc-probe/bear-export.cpp'
serializes the currently materialized 'MetadataUpdate' fields: priority,
extent presence/values/source-size index (serialized as width=size[0],
height=size[2], depth=size[1]), zone/elevation, snap, screen and
distance flags/factors, extended-precision presence/values, per-object trim
disabled state, and divergence state. Fields whose source state is not carried
by 'MetadataUpdate' are explicitly marked as not carried (diffuse, warp, trim
element); no decoder or additional-element parser was expanded.

The focused exporter test 'Eac3BearExportProbe --self-test' passed, including
schema fields and an exact version-2 binary header/layout check. The Python
synthetic test 'py -3.14 -m unittest
tools.atmos-render.test_run_bear_montero_bundle' passed 3/3 cases: old
metadata without schema/extent remains accepted; width, height, and depth
require explicit extent presence; divergence is never copied to BEAR diffuse;
reserved warp, screen anchoring, and infinite distance are audited as
unsupported/ignored.

'tools/atmos-render/run_bear_montero_bundle.py' now maps only explicit
width/height/depth presence to the Python 'ObjectsInput'. It leaves diffuse at
the BEAR default and reports 'adapterAudit' ignored/unsupported counters in
'provenance.json'. The existing MONTERO export remains bounded by
'reservedWarpMode=3' and 'warpRendered=false'; no warp, trim, divergence,
screen-anchor, infinite-distance, priority, zone, or snap sound rule was
invented. Existing export9 audio was not re-rendered in this slice, so the
previous open-reference listening evidence remains the applicable audio
result.

Validation commands and results:

* 'cmake --build build-mm --target Eac3BearExportProbe --config Debug -- /m:4'
  — PASS.
* 'ctest --test-dir build-mm -C Debug -R Eac3BearExportProbe
  --output-on-failure' — 1/1 PASS.
* 'Eac3OamdB1Probe', 'Eac3OamdAdditionalProbe', 'Eac3OamdB2bProbe',
  'Eac3SceneObjectPropertiesProbe', 'Eac3SceneObjectRampSchedulerProbe', and
  'Eac3SceneObjectGainFrameProbe --self-test' — all PASS (15, 14, 37, 47,
  25, and 18 cases respectively).
* 'scripts\test-atmos-property-coverage.ps1' — PASS, 21 rows.
* 'scripts\validate-all.ps1 -BuildDir build-mm -Configuration Debug' —
  PASS (10 unit-test suites, report schema, smoke); aggregate report:
  'build-mm\validation-report.json'.

The next priority remains a standards-backed valid trim/divergence fixture
('warp_mode' 0/1 and ID5) plus an independent object-render oracle. The
MONTERO 'warp_mode=3' path remains unsupported and fail-closed; divergence
must not be treated as diffuse.

## 2026-08-31 JOC PCM/OAMD identity and L/R contribution audit

The read-only audit of the local ETSI references found no normative external
binding of a JOC decoder PCM ordinal to the OAMD 1-based `objectIndex`. TS
103 420 V1.2.1 4.3--4.4 (printed pages 15--18) defines PCM object essences
and corresponding timestamped metadata; 6.3.2.4/6.4 (pages 57--61) defines
the ordinal `Qout(1..joc_num_objects)` array; and 6.6.6 (pages 64--65) uses
`obj=0..joc_num_objects-1` for `z[obj]`. OAMD 5.6.4.8 (pages 47--48)
defines bed/ISF/dynamic ordering, while 5.2.1.4 and Annex B (pages 21 and
73--76) define metadata-to-speaker/ADM conversion. None defines an
objectIndex-to-Qout identity or fixed permutation. TS 102 366 Table 4.3
(page 34) is only channel-based AC-3 ordering. The local untracked
`ts_103420_tables.c` contains Huffman/prototype tables, not object binding.
The source PDFs and table file remain user-provided/untracked; extracted
text and all measurements are ignored local evidence under `tmp/`.

## 2026-08-31 Gate 5A/5B normative vector coverage

Gate 5B now checks all 512 Table 54 subband entries (eight legal band counts
times 64 subbands) against independent hardcoded reference run lengths. It
also covers asymmetric 2-channel/3-band dense and sparse differential
recurrences, plus exact 24-timeslot smooth/steep boundary vectors at offsets
1/12/24, previous-matrix update, and sequence-discontinuity reset. Gate 5A
adds 12 fixed Annex A.1 codeword-to-symbol canaries spanning all six loaded
JOC Huffman tables (generic, sparse coefficient, and 5ch/7ch position).

Focused results are PASS (`mappingCases=512`, `interpolationCases=16`,
`annex-a-fixed-canaries=12`). These are independent syntax/math vectors only;
they do not establish equivalence with an external Dolby decoder or fix the
remaining JOC PCM/object identity question. No production algorithm was
changed.

Reproducible focused command:

```powershell
build-mm\Release\Eac3AccessUnitProbe.exe tmp\oracle\single-object-oracle\ec3\single-object-01.ec3 --max-units 1 --joc-self-test --joc-math-self-test --joc-table docs\dev\ts_103420_tables.c
```

The compact report
`tmp/oracle/lr-slot-audit-r12-final/lr-slot-contribution-summary.json`,
generated by `scripts/audit_lr_joc_contributions.py`, uses the bounded r4
bundles and pinned EAR/BEAR path. It retains per-case/slot 12-tone complex
responses, steady-state energy and official EAR polar positions, plus paths
to the full 158-AU per-slot traces. All six non-zero slots were solo-rendered
for each of L and R. Their linear sum reproduces the full render without
normalization: L max absolute `4.27e-9`, relative RMS `2.38e-7`; R max
absolute `4.86e-9`, relative RMS `2.18e-7`.

Six-slot permutation search improves a case-specific complex-error score but
does not establish identity: L baseline/best is `31.070/19.349` with best
mapping `1->2, 2->4, 3->6, 4->5, 5->1, 6->3`; R is `42.799/32.595` with
best mapping `1<->2`. The best common fixed mapping is only `1<->2`, reducing
the combined score `73.869` to `53.608`, while leaving maximum L/R ILD/IPD
errors `19.682/3.009` and `19.542/2.704` dB/rad. This is not a spatial
equivalence fix. No parser or renderer production patch is justified.

Coordinate conversion and BEAR linearity are therefore not the remaining
L/R explanation. The next priority moves to JOC/QMF PCM decomposition,
timebase/complex coefficient behavior, and a raw decoder-contract probe that
can prove object identity. Do not infer an OAMD permutation from these
diagnostic fits.

## 2026-08-31 QMF synthesis equation conflict locked by dual reference

The candidate TS 103 420 7.3 Pseudocode 14 phase
`(2*j - 2*n - 1)` was tested against an independent scalar implementation.
It differs from the production QMF at max absolute error `0.00554108`,
first mismatch `4`. The production kernel matches the same-page matrix
equation `(2*j - 4*n + 1)` within `1e-6`; ETSI TS 103 190-1 Pseudocode 66
also supports that matrix-equation form. The matrix form therefore remains
production behavior.

Gate 4 with the matrix form is PASS: impulse/sine/sweep/random SNR is
`73.10/83.61/78.24/78.30 dB`, with delay `577` and unity gain within the
reported tolerance. The rejected Pseudocode 14 production candidate produced
`0.20/59.84/0.74/0.25 dB` and inconsistent delays `514/1264` in the same
probe. `qmf-probe` now reports both comparisons and requires matrix-reference
PASS plus explicit `synthesisPseudocode14ConflictObserved=PASS`. No 8-case
bundle rerun was performed because production behavior is unchanged.

## 2026-09-01 Object Direct low-frequency diagnostic

The read-only Riptide audit used the existing Gate6C BSCN bundle and did not
submit audio to a Windows Spatial endpoint. The source was
`E:\Tool\AMDL\downloads\Atmos\The Chainsmokers\So Far So Good\01. Riptide.m4a`
(SHA-256
`8c384ec78eeea608554433cfc1dfa5a3eb73db161bcf91ef6c1f071796bf7fc4`). The
lossless E-AC-3 sidecar has SHA-256
`b06c46202e6a36f1bdf867114cb81c3e9e2073cb912036a2bede4d2f3e0fc3ab`, 5358
access units, and 171.456 seconds of decoded 48 kHz timeline.

`tools/atmos-render/object_direct_oracle.py` provides a renderer-neutral
oracle: it validates 15 planar object channels, applies the current 15 dB
dynamic headroom (`0.177827941`), applies metadata gain/ramp, and sums to
dual-mono stereo. LFE is excluded by default and only included with an
explicit diagnostic switch. It is not a binaural renderer, Spatial endpoint
proof, or listening evidence. The full Riptide report is
`tmp/riptide-spatial-audit-20260901/object-direct-riptide-full-v5-no-lfe/object-direct-report.json`:
peak `0.152777`, RMS `0.033917`; output frequency-band fractions are
`35.12% <=80 Hz`, `25.67% 80-200 Hz`, `27.92% 200 Hz-1 kHz`, `7.51% 1-5 kHz`,
and `3.78% 5-20 kHz`; `clipSamplesAbsGt1=0`. The decoded Riptide LFE is
exactly zero. In the v5 full run, the LFE-excluded and explicitly included
direct WAV SHA-256 are both
`5cda2553c7613b529a534fbdce76f34e7f7ad9b8ca92b9326a84a1af1dc17f78`; the
8-AU v2 pair is likewise identical
(`f60c7b5ea117996f1959d2b85d4e4d210222b234a62e2f03261802b1523ee097`). A
Riptide LFE-off change therefore cannot be attributed to program LFE; the
object PCM itself contains substantial low-frequency content.
The explicit-LFE v5 report is
`tmp/riptide-spatial-audit-20260901/object-direct-riptide-full-v5-lfe/object-direct-report.json`;
it has the same hash because this source's decoded LFE is all zero.

The matching short comparisons for the existing MONTERO bundle are under
`tmp/riptide-spatial-audit-20260901/object-direct-montero-8au/` and
`object-direct-riptide-8au/`. MONTERO has a real but small LFE contribution
(the existing native report gives peak `0.01584676`, RMS `0.00358026`, about
`0.092%` of six-channel energy), so LFE can be an isolated MONTERO A/B
variable, but there is no synchronized loopback or subjective causal proof
here.

For a same-input short decode, `media\MONTERO.ec3` and the Riptide sidecar were
each re-exported with `--max-units 8 --joc-gate6c --joc-bear-export`; the
corresponding bundles and logs are under
`tmp/riptide-spatial-audit-20260901/montero-ec3-8au/` and
`riptide-ec3-8au/`. Both Gate6C and BEAR export passed; the overall probe was
`INCONCLUSIVE` because the QMF smoke coverage is below 1000 access units.
Object Direct produced 12288 frames for each. The compact comparison is
`tmp/riptide-spatial-audit-20260901/object-direct-riptide-montero-8au-comparison.json`
and `.md`; `tools/atmos-render/compare_object_direct.py` recreates it.

`scripts/audit-atmos-spatial.ps1` is a report-first bounded orchestrator. It
defaults to dry mode and never starts capture or Spatial submission. Explicit
`-Submit` starts `WasapiLoopbackCapture` before
`play-atmos-spatial.ps1`, using ready/stop files and timestamped output. A
clean loopback remains INCONCLUSIVE under the endpoint evidence contract.
PowerShell self-test passed (`cases=10`) under Windows PowerShell 5.1 and
PowerShell 7, Object Direct self-test passed (`7 tests`), loopback analyzer
self-test passed (`5 tests`, including PCM32, IEEE-float32, and extensible
IEEE-float32 RIFF formats), existing
`tools/atmos-render` Python tests passed (`6 tests`), and
`SpatialDynamicProbe --self-test` passed (`20 checks`).

The loopback analyzer uses a minimal RIFF/WAVE `fmt`/`data` parser rather than
Python's `wave` module. It distinguishes PCM8/16/24/32 from IEEE-float32/64
and WAVE_FORMAT_EXTENSIBLE PCM/float subformats, rejects unknown tags, and
reports `combinedFrameDownmixStats` (not an interleaved sample-rate-
distorting statistic). Its FFT is 4096-frame Hann-windowed real FFT blocks;
program material remains envelope evidence, not a reverberation-time or
subjective-quality measurement. Analyzer nonzero/unparseable output is
reported by the audit orchestrator as `INCONCLUSIVE_ANALYZER_FAILED`.

Object Direct gain ramps now match `SpatialPropertyAdapter`: audible-to-audible
states interpolate `gainDb` before converting to amplitude, while any
audible/silent transition interpolates linear amplitude. The first state still
snaps immediately; a later update evaluates the prior state at its source
position and interrupts an overlapping ramp. The audit orchestrator also
writes `audit-report.json` on a post-start ready-timeout or delegate exception
and exits nonzero after preserving the specific failure result.

The synthetic `SpatialDynamicProbe` now also accepts `--signal sine|impulse`
and `--position moving|front|left|right|upper`; `--objects 1` remains valid.
The extension is probe-only and was built as Debug and verified by 20/20
self-test checks. No live probe was run in this overnight audit, so it adds no
endpoint-capability or audible-output claim.

## 2026-09-01 JOC low-QMF-band energy accounting

The diagnostic-only matrix trace now records, per AU and for QMF subbands 0
and 1, non-LFE Qin energy, each object's incoherent contribution baseline,
coherent expected energy, actual Qout energy, cross-term energy, maximum matrix
row norm (the unit-input theoretical coherent-gain bound), maximum coefficient,
maximum number of nonzero coefficients in one matrix row, and the number of
timeslots with multiple nonzero inputs. The nonzero threshold is explicitly
`abs(coefficient)>1e-12`. It also records independent time-domain LFE PCM
energy. This is intentionally a compact extension; it does not dump additional
complex arrays. The 48 kHz/64-band QMF has nominal 375 Hz bands, so the trace
reports subband 0 as nominal 0--375 Hz and subband 1 as 375--750 Hz. It cannot
resolve <=80 Hz from 80--200 Hz; those boundaries are not claimed. A separately
synchronized time-domain FFT is required for that split.

The accounting formulas are, for object `o`, input channel `c`, time slot `t`,
and subband `b`, `a = M[o,c,t,b] * Qin[c,t,b]`,
`E_incoherent = sum(|a|^2)`, `E_coherent = sum(|sum_c(a)|^2)`, and
`E_cross = E_coherent - E_incoherent`. `E_actual` is `sum(|Qout|^2)` and is
compared with `E_coherent` through the existing reconstruction check. The
incoherent baseline is useful for exposing coherent addition, but summing 15
object energies is not acoustic energy conservation and is not a loudness or
listening-quality claim.

Representative 1000-AU runs (1,536,000 LFE samples each) used the existing
release probe and `--joc-gate6c --joc-matrix-trace`:

* Riptide evidence is under
  `tmp/riptide-spatial-audit-20260901/joc-lowfreq-riptide-1000-v2/`. The probe
  passed 1000 reconstructed AUs, with finite LFE peak 0 and 1,536,000 zero
  LFE samples. `analyze_joc_low_frequency.py` returned `PASS_ACCOUNTING`,
  reconstruction `maxAbs=0`, mismatches `0`, low-band Qin energy
  `1,068,360.8301`, object actual/coherent/incoherent energy
  `1,070,453.2867`, actual/Qin ratio `1.0019585673`, cross term `0`,
  actual/incoherent ratio `1.0`, maximum row norm `1.30126953125`, maximum
  nonzero coefficients per row `1`, and multi-input row-timeslots `0`.
  Thus crossTerm=0 is explained by every audited active matrix row having at
  most one nonzero coefficient; it is not a claim that all JOC rows are sparse.
* MONTERO evidence is under
  `tmp/montero-media-audit/joc-lowfreq-montero-1000-v2/`. The probe likewise
  passed 1000 AUs and reconstruction `maxAbs=0`, mismatches `0`. Independent
  LFE energy was `1.50000713365` over 1,536,000 samples (`503,552` nonzero);
  low-band Qin energy was `112,735.65456`, object actual/coherent energy
  `117,380.82077`, incoherent baseline `111,700.75821`, cross term
  `5,680.06256`, actual/Qin ratio `1.0412040558`, actual/incoherent ratio
  `1.0508507`, maximum row norm `7.67168708`, maximum nonzero coefficients per
  row `5`, and multi-input row-timeslots `157,486`.

Riptide reports `ZERO_LFE_INPUT_AND_NO_LFE_TERM_IN_FORMULA`: its traced Qout is
fully reconstructed by the non-LFE Qin matrix formula, but zero LFE provides no
excitation for a leakage A/B. MONTERO is the stronger internal control and
reports `NONZERO_LFE_EXCLUDED_FROM_TRACED_QMF_RECONSTRUCTION`: independent LFE
is nonzero while the same non-LFE-only formula still reconstructs Qout with
`maxAbs=0`. This rules out an LFE term in the current traced reconstruction
for those AUs. It does not prove that an upstream decoder never folded content,
external Dolby equivalence, acoustic conservation, endpoint output, or
subjective sound.

The checked-in analyzer is `scripts/analyze_joc_low_frequency.py`; its focused
self-test covers zero-LFE identity, same-phase coherent addition, anti-phase
cancellation, known matrix gain, and known nonzero LFE accounting (5/5 PASS).

The complementary checked-in time-domain analyzer is
`scripts/compare_joc_time_domain_bands.py`. It decodes the source with ffmpeg,
loads the matching Gate6C BSCN object bundle, verifies a known 5.1(side)
layout (or requires explicit channel indices), and uses a 4096-frame Hann/Welch
analysis (11.71875 Hz bins) to report <=80, 80--200, 200--375 and higher bands
for decoded non-LFE channels, independent LFE, raw object channels, and their
coherent sum. It is complementary spectral accounting: object-energy sums are
not acoustic conservation. Riptide and MONTERO both used exactly 1000 AUs /
1,536,000 frames and returned `PASS_ACCOUNTING`. Reports are respectively
`tmp/riptide-spatial-audit-20260901/time-domain-riptide-1000-matching-fnv2/joc-time-domain-band-audit.json`
and
`tmp/montero-media-audit/time-domain-montero-1000-matching-fnv2/joc-time-domain-band-audit.json`.
The Riptide report uses the provenance-bound sidecar bundle
`tmp/riptide-spatial-audit-20260901/matching-ec3-1000-fnv2/bundle`; the MONTERO
report uses `tmp/montero-media-audit/matching-ec3-1000-fnv2/bundle`. These
bundle provenance records use the standard FNV-1a-64 offset basis
`14695981039346656037`; their source digests are respectively
`fnv1a64-6bc7c0bc888afb19` and `fnv1a64-097bf820e6e362ac`.
The analyzer now requires `bundle-provenance.json` with matching absolute
source path and FNV-1a-64 digest; missing or mismatched provenance is rejected.
`--allow-unverified-bundle-source` is available only for exploratory runs and
forces `INCONCLUSIVE_UNVERIFIED_BUNDLE_SOURCE`.
The native BEAR exporter now writes this provenance file when given the input
path (the existing no-source API remains compatible but marks the bundle
unverified); its focused schema test passes.
Riptide decoded main/LFE/object low-80 energies were `1.038203/0/1.040006`,
while matching MONTERO was `0.652755/0.001596/0.656491`; corresponding
decoded-main / LFE / raw-object energies in 80--200 Hz were Riptide
`13.494817/0/13.519710` and matching MONTERO
`1.921207/0.002416/1.957863`. Raw-object/main ratios in <=80 Hz and 80--200 Hz
are respectively Riptide `1.001737/1.001845` and MONTERO `1.005723/1.019080`;
coherent-object/main ratios are Riptide `1.990301/1.871293` and MONTERO
`2.264047/2.148313`. This indicates high internal object coherence, not endpoint
measurement or a bug proof. The MONTERO bundle was generated by the same
1000-AU probe from `media\\MONTERO.ec3` (source SHA-256
`c91a662edd693d8a909e1780d9dea2831624fbf19ba3b8ed5e5a9c6fce81ff9e`), with
verified bundle provenance FNV `fnv1a64-097bf820e6e362ac`; decoded and bundle
LFE were exactly aligned (`maxAbsDifference=0`). These time-domain values
resolve frequency boundaries but do not establish endpoint output or listening
quality.

## 2026-09-01 Spatial endpoint impulse and bounded Riptide capture

The initial three-position run used a zero-frame impulse and was dominated by
endpoint startup. Its independent captures at
`tmp/spatial-live-20260901/spatial-impulse-20260901T034257394Z/audit-report.json`
selected the endpoint with capacity 128 and all returned internal probe PASS,
but loopback peak/RMS were zero. It was correctly recorded as
`INCONCLUSIVE_LOOPBACK_SILENT` / `STOP_NO_VALID_TRANSIENT`; no position or tail
claim was made from that run.

The checked-in impulse orchestrator remains dry by default and requires
`-Submit`; its Windows PowerShell self-test is 8/8, and the position comparator
self-test is 4/4 (including per-capture alignment and partial-silence rejection).

The impulse startup timing was then corrected without changing the production
path: `SpatialDynamicProbe` gained optional `--impulse-delay-ms` (default 0),
and its impulse amplitude is now 0.1 while sine remains 0.018. The orchestrator
uses a 300 ms impulse delay, a 1,500 ms capture, and waits 200 ms after capture
ready. The three-position rerun is at
`tmp/spatial-live-20260901-delay300/spatial-impulse-20260901T035202395Z/`.
All probes passed with capacity 128 on the same endpoint, and the loopback
finally contained a measurable transient: front/left/upper onsets were
352.646/350.458/352.604 ms, peaks 0.05962/0.09668/0.06056, and left/right
energy ratios 1.0004/34.3447/1.0001. After aligning each capture at its own
first valid transient, pairwise peak-normalized correlations were -0.0067
(front/left), -0.1801 (front/upper), and 0.0113 (left/upper), with 41,258--41,633
common post-onset frames. The corrected comparison is
`tmp/spatial-live-20260901-delay300/spatial-impulse-20260901T035202395Z/position-comparison-corrected.json`.
The comparison result is `PASS_POSITION_DIFFERENTIATED_ENDPOINT_CAPTURE`,
but remains limited to the loopback layer; it is not a physical headphone or
subjective localization claim.

The bounded Riptide/Object Direct comparison was rerun without playback after
fixing band energy to average per-channel power before normalization. The v3
report is
`tmp/spatial-live-20260901/riptide-500au-fixed/Riptide-20260901T034350848Z/loopback-object-direct-comparison-v3.json`.
Over the analyzed 0--20 kHz bandwidth, Loopback/Object Direct fraction ratios
(0--80, 80--200, 200--375, 375--1000, 1000--5000, 5000--20000 Hz) were
0.5976/0.6846/1.1324/1.0299/0.6902/0.6914 (dB:
-2.24/-1.65/+0.54/+0.13/-1.61/-1.60). These are normalized spectral shape
diagnostics only; absolute FFT energy is not used as a verdict, and the Object
Direct dual-mono signal is not a binaural or endpoint reference.

The matching Riptide sidecar (SHA-256
`b06c46202e6a36f1bdf867114cb81c3e9e2073cb912036a2bede4d2f3e0fc3ab`) was then
submitted for 500 AU with LFE disabled:
`tmp/spatial-live-20260901/riptide-500au-fixed/Riptide-20260901T034350848Z/`.
Gate6C, Gate7B, endpoint submission, and the 500-AU delegate completed; the
provenance records `lfePolicy=DISABLED_BY_USER`, `lfeVolume=0`, and positive
`dynamicGainHeadroomDb=15`. The 16.23 s loopback was non-silent and parsed as
Extensible float32: channel peaks/RMS were approximately `0.3110/0.0532` and
`0.3099/0.0497`, with zero clipped samples. The audit result remains
`INCONCLUSIVE_ENDPOINT_OUTPUT_NOT_PROVEN`: the internal submission PASS and a
non-silent loopback sidecar do not establish spatial correctness or listening
quality. No further night-time playback was attempted.
The underlying 500-AU native log still ends with
`jocQmfResult=INCONCLUSIVE ... coverage-below-1000-access-units` and records
`probeExitCode=1`; the wrapper's endpoint provenance is PASS because Gate6C,
Gate7B, and endpoint submission completed. This distinction is retained
instead of treating the bounded run as a full QMF-coverage PASS.

The loopback analyzer now accepts the actual RIFF little-endian GUID layout
for WAVE_FORMAT_EXTENSIBLE PCM/IEEE_FLOAT; its self-test is 5/5. This fixes
capture parsing only and does not change production rendering behavior.

For the same bounded Riptide window, the renderer-neutral Object Direct
reference was generated from the matching 500-AU bundle at
`tmp/spatial-live-20260901/riptide-object-direct-500au/` and compared after
aligning loopback onset sample `13967` (`765073` common frames). The comparison
report is
`tmp/spatial-live-20260901/riptide-500au-fixed/Riptide-20260901T034350848Z/loopback-object-direct-comparison.json`.
Loopback/reference mono peak and RMS were `0.30563365/0.04946308` and
`0.12177522/0.02128722`; RMS-normalized correlation was `0.01010` and
difference RMS `1.40706`. This is a path-shape diagnostic only: startup,
endpoint processing, device routing, and the non-binaural Object Direct
reference prevent a sample-identity or quality interpretation.
