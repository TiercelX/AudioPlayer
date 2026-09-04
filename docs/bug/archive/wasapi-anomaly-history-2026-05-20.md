# Archived WASAPI anomaly history through 2026-04-29

This archive was split from `docs/bug/wasapi-anomaly-status.md` on 2026-05-20 to keep the active tracker compact for future context windows. Current guidance remains in the active tracker and `docs/dev/*.md`.

## Manual repro notes

- 2026-04-29 segmented loopback manual evidence after `b08a563`:
  no-switch silence baseline used `silence-48k-stereo.wav` with a 10 second
  observation window. Harness and app report were both `PASS`; the render mirror
  was observed and clean, submitted PCM discontinuity and internal glitch
  candidates were false, there was one loopback segment, capture was not
  interrupted, and `loopbackTransientCandidateCount=0`. Current read: the
  segmented loopback wrapper does not appear to create false transient evidence
  in this baseline.
- 2026-04-29 silence spatial-switch evidence: Off -> Windows Sonic and Windows
  Sonic -> Off both used `silence-48k-stereo.wav` with a 15 second manual
  observation window. Both runs reported app `WARN`, harness/wrapper
  `INCONCLUSIVE`, render mirror observed/clean, no submitted PCM discontinuity,
  no stale session write/read, no old PCM leak, and no internal glitch
  candidates. Off -> Windows Sonic observed two waiting-for-invalidation events,
  two same-output invalidations, one absorbed event, one conservative rebuild,
  and one active-switch rebuild/preflight. Windows Sonic -> Off observed one
  waiting-for-invalidation event, one same-output invalidation, one absorbed
  event, one conservative rebuild, and one active-switch rebuild/preflight. Both
  had `systemInvalidationDuringSwitch=true`, two loopback segments, interrupted
  capture with `loopbackInterruptionReason=device-invalidated` and HRESULT
  `0x88890004`, and `loopbackTransientCandidateCount=0`. Current read: spatial
  mode switching consistently causes same-output endpoint/client invalidation,
  while the submitted PCM and render-mirror layers remain clean. A zero loopback
  transient count is inconclusive when the loopback client is interrupted at the
  critical window.
- 2026-04-29 tone spatial-switch evidence: Windows Sonic -> Off during
  `sine-1khz-minus30db-48k-stereo.wav` also reported app `WARN` and harness
  `INCONCLUSIVE`; render mirror was clean, submitted PCM discontinuity/stale
  data/old PCM leak/internal glitch candidates were false, and same-output
  invalidation interrupted the loopback capture with device-invalidated HRESULT
  `0x88890004`. Reported levels included `maxAudioPeak=0.004`,
  `maxSubmittedPcmPeak=0.0021`, and `maxSubmittedPcmJump=0.0003`. At low system
  volume, the -30 dB sine was too quiet for reliable manual judgment. With the
  Windows/soundcard endpoint volume raised to 100 and Steam speaker at 100, the
  operator heard previous-audio residue before playback start, shaking/jitter
  during playback, and a clear pop when switching Windows Sonic back to Off.
  Treat volume as a reproducibility factor, not as a demonstrated root cause.
- Current 2026-04-29 classification: likely Windows endpoint/APO/driver-side
  spatial-switch transient unless future evidence shows submitted PCM or
  render-mirror corruption. Current runs do not show player-submitted PCM
  corruption. Keep decoder/internal PCM, submitted/render PCM, loopback capture,
  and physical endpoint output as separate evidence layers. Do not treat
  `loopbackTransientCandidateCount=0` as proof of no endpoint pop when capture
  was interrupted by endpoint invalidation. Do not change
  `WindowsWasapiAudioPlayer` playback behavior from this evidence alone.
- Optional evidence improvements remain diagnostic-only: add a manual
  observation sidecar JSON written after a run or from optional parameters, and
  add pre-playback loopback pre-roll so endpoint output before app playback start
  can be captured for the previous-audio residue report. Low-risk mitigations,
  if later justified, should be labeled as mitigations rather than fixes, for
  example an optional post-invalidation silence guard or short fade-in.
- Current anti-regression guardrails: do not reintroduce speculative mute, duck,
  or fade on plain `audioOutputsChanged` or same-device output menu
  notifications; do not reintroduce entry-time speculative taper; do not enable
  `AUDIOPLAYER_SPATIAL_ENDPOINT_FLUSH` by default; do not change the app JSON
  report contract; do not claim Windows Sonic switching pop is fully fixable
  from app code.
- 2026-04-29 seek-resume investigation: manual progress-slider seeking pauses
  playback before `seek()`, so the backend previously tore down the paused
  pipeline and the immediate resume entered `NormalStart` instead of a seek
  startup profile. The diagnostic package
  `player-20260429-184345-102-57268` showed session 1 clean, then repeated
  seek/resume sessions with submitted PCM artifacts under `NormalStart`, plus
  heavy decoder stdout backpressure while the PCM buffer was full. This is an
  app-side seek/restart pipeline issue, separate from the Windows Sonic endpoint
  conclusions above.
- Seek-resume mitigation scope: use a dedicated `SeekResume` startup profile for
  paused-slider seek release and scripted seek-resume automation, keep old
  decoder output quarantined before the new session starts, and pace WASAPI
  ffmpeg decode in real time by default so Debug seek tests do not accumulate
  tens of MB of pending stdout. `SeekResume` may use a bounded
  `-readrate_initial_burst 1.500` after the target is fixed; this is decoder
  priming, not playback-rate control or drag-time prefetch.
- Large seek-resume slice: ALAC `SeekResume` can now use an in-process libav
  decoder worker (`decoderMode=libav-seek`) instead of the ffmpeg CLI stdout
  path. The new worker is limited to the WASAPI ALAC seek-resume startup path
  and feeds the existing PCM buffer/output worker; normal start, source switch,
  active-switch rebuild, error recovery, and non-ALAC paths keep the existing
  CLI decoder route. Use `AUDIOPLAYER_WASAPI_LIBAV_SEEK_RESUME=0` to force the
  previous CLI behavior for comparison. This is intended to remove process
  pacing/backpressure variance from ALAC seek recovery; it does not prove
  physical endpoint output is pop-free.
- 2026-05-05 libav seek-resume validation: Debug build passed. Repeated
  `sine-1khz-minus18db-48k-stereo-alac.m4a` SeekResume used
  `decoderMode=libav-seek`, passed with `seekResumeLatencyMs=65`, clean
  render mirror, no decoder backpressure, no underrun, no stale buffer/session
  access, and no submitted PCM discontinuity. The real local
  `real-alac-sample.m4a` evidence-only run used `decoderMode=libav-seek` and
  reduced seek latency to `67 ms` with no backpressure/underrun/stale access,
  but still reported compressed-content `SeekResumeBoundary` candidates and
  remains `INCONCLUSIVE`; do not claim real ALAC audible instability is fixed
  without manual listening or loopback evidence.
- 2026-05-06 libav decoder scope update: WASAPI now routes validated packaged
  codecs through the in-process libav worker when available: AAC, ALAC, FLAC,
  MP3, and PCM WAV variants. Diagnostics report this as
  `decoderMode=libav-inprocess`; unsupported codecs, Dolby codecs pending clean
  libav-worker validation, or explicit opt-out use `decoderMode=ffmpeg-cli`.
  Use `AUDIOPLAYER_WASAPI_LIBAV_DECODER=0` to force the CLI path. The older
  `AUDIOPLAYER_WASAPI_LIBAV_SEEK_RESUME=0` opt-out is still honored. AC3 and
  E-AC-3 libav smoke attempts on 2026-05-06 reported submitted-PCM artifacts
  while AC3 CLI comparison passed, so Dolby remains on the CLI route for now.
- 2026-05-06 first-seconds playback investigation: the suspected artifact was
  re-scoped away from quit/teardown and into the first few seconds after
  playback start. A temporary 6000 ms WASAPI render-mirror window was enabled via
  `AUDIOPLAYER_WASAPI_RENDER_MIRROR_WINDOW_MS=6000`. Synthetic ALAC sine stayed
  clean through submitted PCM, render cadence, buffer-level diagnostics, and
  uninterrupted loopback capture (`loopbackTransientCandidateCount=0`). The real
  ALAC sample produced loopback transient candidates, but direct source decode
  contained dense matching music-content transients; with about 400 ms endpoint
  alignment, 39/41 libav-loopback candidates matched direct-source candidates.
  The ffmpeg CLI opt-out comparison produced the same endpoint candidate shape
  (40 candidates). Current read: this evidence does not support a libav-specific
  first-seconds submitted-PCM corruption or buffer starvation bug. If a manual
  listener still hears a non-content pop in this window, capture the exact source
  and timestamp; treat it as endpoint/manual evidence until loopback or
  submitted-PCM evidence separates it from source content.
- 2026-05-06 unstable-platform mitigation: a UI-selectable WASAPI stability
  mode was added as an opt-in fallback for machines that show system real-time
  audio pressure, for example LatencyMon DPC/ISR warnings, hard pagefault bursts,
  or suspected BIOS/voltage/power-management instability. This is a mitigation,
  not a pop/click root-cause fix. When enabled, the player stays in WASAPI shared
  mode, disables the exclusive-mode request, raises the audio worker thread
  priority, requests a 500 ms shared WASAPI buffer, uses a 500 ms normal-start
  prefill threshold, expands the PCM ring buffer to about 4 seconds, and uses a
  200 ms SeekResume prefill threshold so seek is covered without making slider
  release as heavy as cold start. Default playback behavior is unchanged when
  the option is off.
- Validation for the stability-mode change: Debug build passed with the existing
  `VCINSTALLDIR` deploy warning. Default sine smoke passed. Stability-mode sine
  smoke passed and logged `stabilityMode=1`, `sharedBufferDurationHns=5000000`,
  and `bufferFrames=24000`. Default repeated SeekResume smoke passed.
  Stability-mode repeated SeekResume smoke passed and logged normal-start
  `startupThresholdBytes=192000` and SeekResume `startupThresholdBytes=76800`
  for 48 kHz/16-bit/stereo, corresponding to about 500 ms and 200 ms
  respectively. These are submitted-PCM/runtime stability checks only; they do
  not prove actual endpoint output is pop-free.
- Current 2026-05-06 playback-risk read: no known remaining "large" app-side
  fault is currently indicated by the latest local evidence. The player now has
  explicit session/generation ownership, stale-buffer guards, conservative
  active-switch rebuilds, submitted-PCM/render-mirror diagnostics, in-process
  decoder routes for validated codecs, SeekResume mitigation, loopback/manual
  evidence tooling, and the new stability-mode fallback. Continue treating
  large pop, playback reset, silence/no-sound, crash, failure to recover,
  submitted-PCM discontinuity, stale session/buffer access, underrun, or decoder
  backpressure as app-side regressions. Light pops under Windows spatial audio,
  endpoint/APO invalidation, or LatencyMon-level DPC/ISR pressure remain
  endpoint/system-layer risks unless future logs show a player-side fault.
- Validation: Debug build passed with the existing `VCINSTALLDIR` deploy
  warning. Repeated seek-resume smoke on
  `sine-1khz-minus18db-48k-stereo.wav` passed with 4/4 `SeekResume` profiles,
  render mirror clean, no stale reads/writes, no underrun, and no backpressure.
  The same repeated seek-resume smoke on `real-alac-sample.m4a` still reported
  submitted PCM artifacts under `SeekResume`; backpressure was gone and the
  remaining hits include source/decoder-shaped ALAC crackle/jump entries, so do
  not claim audible ALAC jitter is fixed without another manual listening or
  loopback run.
- 2026-04-30 seek-resume diagnostic judgment update: `real-alac-sample.m4a` is
  a real music sample, not a synthetic oracle. Seek-resume reports now separate
  `SeekResumeBoundary` candidates from `FullSegment` content transients and add
  `artifactClassification`, `firstArtifactOffsetMsAfterResume`,
  `seekResumeBoundaryWindowMs`, and `compressedContentSample`. Sine/silence
  fixtures remain hard pass/fail assertions. Real ALAC/music samples should not
  fail solely on full-window transient counting; future ALAC seek bugs should be
  judged by manual audibility plus boundary-window evidence or proven stale
  PCM/submitted discontinuity.
- 2026-05-01 SeekResume boundary follow-up: first submitted-block diagnostics
  now log `firstSubmittedBlockPeak`, start/end sample, fade applied, min/max
  fade gain, and first-50 ms submitted/render-mirror metrics. The 12 ms
  SeekResume fade was confirmed to reach full gain inside the first 1056-frame
  submitted block; SeekResume only now uses a zero-start PCM fade and a 24 ms
  fade duration. Repeated sine SeekResume smoke passed with
  `seekResumeFadeInMs=24`, `firstSubmittedBlockMinGain=0`,
  `firstSubmittedBlockMaxGain=0.916594`, clean render mirror, no submitted PCM
  discontinuity, no underrun, and no stale session/buffer access. This remains
  submitted-PCM-layer evidence, not proof of physical endpoint pop/click
  absence.
- Case A, Off -> Windows Sonic: one manual spatial-audio switch; light pop heard
  on recovery. Harness was `INCONCLUSIVE`. Diagnostics showed active output
  switch detected, two active-switch starts, one completion, same-output
  invalidation detected with two invalidation transactions and one absorbed
  event, and `systemInvalidationDuringSwitch=true`. WASAPI recovery, underrun,
  old PCM leak, stale buffer reuse, submitted PCM discontinuity, and internal
  glitch candidates were not detected; render mirror was clean. Current read:
  enabling Windows Spatial Audio can trigger multiple same-output invalidation
  events while internal PCM/stale/underrun/recovery diagnostics stay clean.
- Case B, Windows Sonic -> Off: one manual spatial-audio switch; light pop heard
  at the switch instant, none after recovery. Harness was `INCONCLUSIVE`.
  Diagnostics showed one active-switch start, one completion, one same-output
  invalidation transaction, one absorbed event, and
  `systemInvalidationDuringSwitch=true`. WASAPI recovery, underrun, old PCM
  leak, stale buffer reuse, submitted PCM discontinuity, and internal glitch
  candidates were not detected; render mirror was clean. Current read: disabling
  Windows Spatial Audio looks more like an endpoint/session invalidation
  transient than player-submitted PCM corruption.
- Shared conclusion: both directions hit system invalidation during active
  switching, while automated diagnostics did not find submitted PCM
  discontinuity, stale buffer reuse, old PCM leak, underrun, WASAPI recovery, or
  internal glitch candidates. This currently points toward a light endpoint
  pipeline rebuild pop. Keep `endpointOutputVerified=false` until loopback
  evidence exists; manual listening remains a note, not automated endpoint
  verification.

## Historical validation notes

2026-04-21 validation notes:

- `scripts/build-app.ps1 -BuildDir build-mm -Configuration Debug` passes; deployment still prints the existing `VCINSTALLDIR` warning.
- `scripts/run-playback-smoke.ps1` seek smoke passed and confirmed `startupProfile=generationBoundary`.
- `scripts/run-playback-smoke.ps1` pause/resume smoke passed and confirmed `resumeContinuity`; `analyze-audio-artifacts.ps1` reported no audio artifacts for that run.
- Forced output refresh smoke passed and confirmed the conservative rebuild path enters `startupProfile=outputSwitchRebuild`.
- Active system device-change follow-up: repeated `audioOutputsChanged` notifications during `SystemSwitchWindowPhase::Restarting` were traced to duplicate `systemSwitchWindowRebuild` starts and generation advances; those restart-window notifications now verify the resolved output before rebuilding instead of suspending the fresh output immediately.
- Post-fix validation passed for forced `SystemDeviceChange` conservative rebuild and ordinary `DeviceRefresh` hot reconfigure smokes; both analyzer runs reported no audio artifacts.
- On `real-alac-sample.m4a`, the analyzer can still report steady-state short-burst/crackle hits after the transition; those remaining hits are not being treated as evidence to expand the detector in this task.
- 2026-04-21 follow-up: ALAC seek residuals were checked against steady-state render cadence and direct ffmpeg PCM output. The WASAPI event cadence stayed stable at one 480-frame period per callback (`padding=576 available=480 write=480` at 48 kHz), with no wait-for-data/underrun signal. Direct ffmpeg decode of the same 45s region also contains dense adjacent-sample jumps, so the remaining steady-state hits are source-content-shaped rather than a newly identified recovery boundary.
- 2026-04-21 P0 silent-output fix: active same-output invalidation now treats stopped/no-error and stopped/error absorption as a conservative rebuild boundary by requiring a fresh buffer and blocking hot reconfigure for that transaction. The same-output invalidation watch also falls back to rebuild instead of resetting the transaction if output is no longer active.
- Validation: `scripts/build-app.ps1 -BuildDir build-mm -Configuration Debug` passes; deployment still prints the existing `VCINSTALLDIR` warning. `scripts/run-playback-smoke.ps1` with forced output refresh on `media/sample-alac.m4a` passed with audible levels and `output-active` after hot reconfigure. `scripts/analyze-audio-artifacts.ps1` could not be used in this checkout because PowerShell reports a parser error in the script around line 218.

2026-04-22 active-switch startup baseline:

- Baseline commit: `fbf1300` (`Use short active switch startup smoothing`) on `codex-0416`.
- `startPipeline` now uses explicit `PipelineStartupProfile` values. The relevant profiles for this baseline are `ActiveSwitchRebuild` and `ErrorRecovery`; do not treat controlled active switching as an implicit error-recovery restart.
- `ActiveSwitchRebuild` is the conservative fallback for active output-switch transactions when hot reconfigure is unsafe or blocked. It creates a fresh pipeline/session/buffer and completes through the active-switch `WaitingForOutputStart` -> `output-active` path.
- Current `ActiveSwitchRebuild` startup behavior: `startMutedForFadeIn=true` when rebuilding during playback, `startupSilence=true`, `startupSilenceMs=8`, and `warmupDiscard=false`.
- Purpose of `ActiveSwitchRebuild` startup behavior: provide only a light first-packet smoothing boundary after the controlled rebuild, while avoiding recovery-style warmup discard and avoiding the longer 16 ms startup silence that risked making the switch feel like an extra mute.
- `ErrorRecovery` remains intentionally heavier: `startMutedForFadeIn=true`, `startupSilence=true`, `startupSilenceMs=48`, and `warmupDiscard=true` with the existing 32 ms recovery warmup discard. It remains for WASAPI errors, device invalidation, and output resource failures, and exits when playback position advances enough to reset recovery state.
- Manual retest after `fbf1300`: P0 silent lock did not return; switching to spatial-audio "none" did not reintroduce the extra silent transition; overall behavior is much better than the initial state.
- Known tradeoff: a light pop can still occur at either the switch instant or the restore/startup side. The current 8 ms `ActiveSwitchRebuild` startup smoothing is an acceptable stage baseline. Do not continue immediate 4 ms / 12 ms / other smoothing-parameter probing without new evidence.
- Current non-goals: no detector expansion, no old `generationBoundary`/window semantics, no render/write cadence work, and no broader transaction refactor.

2026-04-22 minimal artifact tracking realignment:

- `AudioArtifactMonitor` is reattached only at the current WASAPI render-worker output path: startup silence submission, recovery warmup silence submission, and real PCM blocks submitted from `renderAvailableFrames`.
- Current artifact attribution is based on `pipelineStartProfile` / `artifactPath` values for `ActiveSwitchRebuild`, `ErrorRecovery`, and `SeekRestart`; old `generationBoundary`, `systemSwitchWindowRebuild`, and startup-profile-window analysis is not part of the restored tool path.
- `scripts/analyze-audio-artifacts.ps1` should be used to summarize the current profile/path fields and nearby active-switch or recovery logs before making playback behavior changes.

2026-04-22 ActiveSwitchRebuild first-block guard:

- The remaining manual-switch artifacts primarily landed on `ActiveSwitchRebuild` / `OutputFormatChange` after `WaitingForInvalidation` absorbed `IOError`, then conservative rebuild completed through 8 ms startup silence.
- Keep `ErrorRecovery`, startup silence duration, detector types, render cadence, and active-switch transaction structure unchanged for this step.
- The narrow mitigation is a one-shot entry guard for the first real PCM block after `ActiveSwitchRebuild`: when that first block is large enough that the existing PCM fade would exceed 50% gain by block end, extend only the PCM fade frame count enough to cap that first-block endpoint. This does not add another silent interval.
- Follow-up automation scope: use the smoke-only `SameOutputInvalidationIoError` scripted trigger for repeatable A/B runs. It drives `OutputFormatChange -> WaitingForInvalidation -> absorbed-output-error -> conservative-rebuild -> ActiveSwitchRebuild` without system-level spatial-audio UI automation, and `run-playback-smoke.ps1` can assert effective conservative rebuild, first-data-block, and first-block guard counts.
- A/B baseline audio should use generated fixtures rather than songs: primary `build-mm/fixtures/ab-pink-noise-48k-stereo-minus18db.wav`, auxiliary `build-mm/fixtures/ab-sine-997hz-48k-stereo-minus18db.wav`. Use 10 effective conservative rebuilds with a fixed trigger interval before comparing first-block artifact counts and first-block `jump` / `peak` / `rms`; treat total artifact counts as weak context only.
- 0.50 guard pink-noise baseline runs on 2026-04-22 both reached 10/10 effective rebuilds, 10/10 first data blocks, and 10/10 first-block guard hits. Both runs produced zero `audioArtifact` entries. First-block metrics were stable: run 1 peak avg/max `0.0187/0.0260`, jump avg/max `0.0119/0.0130`, rms avg/max `0.0043/0.0062`; run 2 peak avg/max `0.0196/0.0231`, jump avg/max `0.0122/0.0132`, rms avg/max `0.0044/0.0050`.
- Manual listening with constant pink noise can still hear a slight switch pop under the 0.50 guard. Treat `audioArtifact total=0` and stable first-block metrics as a reproducible A/B baseline, not proof that subjective pop is gone. Before tuning the guard lower, inspect the `activeSwitchBoundaryEnvelope` log that correlates the previous rendered block, configured startup silence, first real PCM block, and first-block fade endpoint for each scripted `ActiveSwitchRebuild`.
- `activeSwitchBoundaryEnvelope` validation on 2026-04-22 reached 10/10 effective rebuilds and 10/10 boundary-envelope lines on the pink-noise scripted trigger, while `audioArtifact total` stayed zero. In that run every boundary had a valid previous rendered block, 384 startup-silence frames, 1056 first real PCM frames, and first-block fade endpoint `0.5000`; this supports treating the remaining subjective pop as an ActiveSwitchRebuild boundary-envelope issue that current artifact detectors do not fully represent.
- 2026-04-23 follow-up: `ActiveSwitchRebuild` entry bridge now replaces the 384-frame startup-silence submission with fresh PCM when available, and falls back to the old silent release with `activeSwitchEntryBridge fallbackToSilence=1` when fresh PCM cannot be submitted. Manual pink-noise spatial-audio switching still produced a slight pop while all bridge lines showed `streamGainAtBridge=0.000`, so the bridge was being queued under stream-level mute. The narrow follow-up keeps the same 8 ms window and first-block endpoint guard, but raises stream gain before releasing a successful bridge buffer; validation log `player-smoke-20260423-175541-682-317cf14b.log` reached 10/10 bridge lines, 10/10 boundary-envelope lines, `streamGainBeforeBridge=0.000`, `streamGainAtBridge=1.000`, and `audioArtifact total=0`.
- 2026-04-23 timing follow-up: `activeSwitchRebuildTiming` logs now bracket the conservative rebuild startup path from `startPipeline` teardown through output configuration, decoder start, first decoder data, start-output readiness, `startOutput` prerender, and entry bridge submission. Validation log `player-smoke-20260423-181142-557-9d71768a.log` reached 10/10 rebuilds and showed `absorbed-output-error -> activeSwitchEntryBridge` at 104-118 ms, with `decoder-start-queued -> decoder-data-available` at 53-63 ms and `start-output-ready -> activeSwitchEntryBridge` at 0-3 ms. This keeps the current evidence focused on rebuild startup latency and decoder/buffer priming, not first-block spike or bridge fallback.
- 2026-04-23 output-start-error follow-up: manual spatial-audio preset switching exposed a separate active-switch hole where a rebuilt `ActiveSwitchRebuild` session could receive `IOError` while still in `WaitingForOutputStart`, before `activeSwitchEntryBridge` ran, and the old branch treated that as fatal `output-error`. The narrow fix keeps this inside the active-switch transaction, logs `activeOutputSwitch absorbed-output-start-error`, requires a fresh buffer, retries one conservative rebuild, and only falls back to the existing fatal path if the retry also fails.
- 2026-04-23 spatial-preset timing follow-up: manual switching suggests presets that enable spatial effects can produce longer repeated invalidation windows than switching back to no spatial effect. The current instrumentation now adds `activeOutputSwitch output-active-summary` for transaction elapsed time, last absorbed-error-to-active time, rebuild time, watchdog rebuild count, and output-start retries, plus `activeSwitchBoundaryPopCandidate` alongside `activeSwitchBoundaryEnvelope` to quantify previous real PCM -> entry bridge -> first block envelope steps without changing playback behavior.
- 2026-04-26 exit-taper follow-up: manual single-switch logs showed `activeSwitchBoundaryPopCandidate candidate=1` in both directions, while `audioArtifact total` remained zero. The narrow mitigation now applies an ActiveSwitch-only same-output invalidation taper before `WaitingForInvalidation` by reducing the old stream gain to `0.45` until either invalidation arrives or the watchdog restores it. This does not change startup silence, entry bridge, or recovery semantics. Validation: `scripts/build-app.ps1 -BuildDir build-mm -Configuration Debug` passed, and the existing pink-noise scripted trigger still passed 10/10 rebuilds with zero playback errors; that scripted path does not exercise the new manual spatial-audio invalidation taper.
- 2026-04-26 one-shot taper follow-up: the same-output invalidation taper no longer holds the old stream at `0.45` for the whole invalidation wait. Entry into the manual same-output invalidation wait now applies a short ActiveSwitch-only duck to `0.20`, holds it for 24 ms, and restores automatically; observed stop/error keeps a final prepare hook for non-terminal paths. This keeps the real WASAPI path from reaching terminal invalidation before any taper, while avoiding the audible long half-volume old-output segment reported after `95a460a`. Validation: `scripts/build-app.ps1 -BuildDir build-mm -Configuration Debug` passed; `scripts/run-playback-smoke.ps1` with `SameOutputInvalidationIoError` on `build-mm/fixtures/ab-pink-noise-48k-stereo-minus18db.wav` passed 10/10 `WaitingForInvalidation`, 10/10 conservative rebuilds, 10/10 `firstDataBlockAfterConfigure`, 10/10 `activeSwitchBoundaryEnvelope`; `scripts/analyze-audio-artifacts.ps1` reported `audioArtifact total:0` for `player-smoke-20260426-222109-312-aaa8c5cb.log`.
- 2026-04-26 terminal position reset follow-up: manual `none -> Windows Sonic` testing with Win11 output controls showed playback restarting from the beginning. Log `player-20260426-222402-508-35232.log` showed the active switch had a valid `currentPositionMs=7100`, then the old terminal session emitted `positionMs=0` after `IOError`, causing `ActiveSwitchRebuild startPositionMs=0`. The fix ignores only active-switch terminal position resets from the current session while the transaction is `WaitingForInvalidation` or `Applying`; normal active position ticks still update position. The same log also showed repeated same-device `audioOutputsChanged` notifications repeatedly running `selectOutputFormat` while `IOError` waited in the event queue, so repeated same-device notifications are now short-circuited while the active switch is already waiting for invalidation or output start. Validation: `scripts/build-app.ps1 -BuildDir build-mm -Configuration Debug` passed; `scripts/run-playback-smoke.ps1` with `SameOutputInvalidationIoError` on `build-mm/fixtures/ab-pink-noise-48k-stereo-minus18db.wav` passed 10/10 rebuilds, 10/10 `firstDataBlockAfterConfigure`, 10/10 `activeSwitchBoundaryEnvelope`; `scripts/analyze-audio-artifacts.ps1` reported `audioArtifact total:0` for `player-smoke-20260426-222831-564-126e4753.log`.
- 2026-04-26 speculative taper rollback: follow-up log `player-20260426-224929-001-10224.log` showed the remaining audible "tap" events came from speculative entry tapers on same-output notifications that later reset as `unchanged-output-no-invalidation`, including one after `output-active`. The actual terminal invalidation had already latched `wasapiError` before the entry taper could affect the old stream. The entry-time taper is therefore removed; only the observed stop/error prepare hook remains for non-terminal invalidation paths. This is intended to remove Win11 output menu click dips and post-rebuild double ducking without changing `ActiveSwitchRebuild`, entry bridge, or recovery semantics.
- 2026-04-27 sample-rate policy follow-up: manual log `player-20260427-021710-263-55020.log` showed a real `48000 -> 192000` `SystemDeviceChange` active switch. The old implementation scaled frame counts by sample rate (`startupSilenceFrames=1536`, `pcmFadeFrames=6144`) but kept the same `ActiveSwitchRebuild` policy shape (`startupSilenceMs=8`, first-block endpoint `0.5000`). The current change adds an explicit ActiveSwitch boundary policy selected from the current/target output rates. `standard` keeps the 48 kHz behavior; `high-sample-rate` applies when either side is at least 88.2 kHz and uses `startupSilenceMs=12`, `pcmFadeInMs=48`, and `firstBlockMaxFadeGain=0.35`, while keeping `warmupDiscard=false` and staying separate from `ErrorRecovery`. `scripts/analyze-audio-artifacts.ps1` now reports recent active-switch boundary policy selections. Local validation passed for build and the existing 48 kHz `SameOutputInvalidationIoError` smoke path; 192 kHz manual validation remains user-side because the current Moonlight server environment cannot change the Windows output format.
- 2026-04-27 manual sample-rate/bit-depth retest follow-up: logs `player-20260427-024604-901-54492.log` and `player-20260427-024925-894-37808.log` confirmed the high-sample-rate policy is selected for 88.2/96/176.4/192 kHz active switches, but audible artifacts remain. The first log reported 42 `audioArtifact` entries and the second reported 4. The 88.2 -> 96 kHz restore anomaly in the second log stayed inside `ActiveSwitchRebuild:SystemDeviceChange`; no `ErrorRecovery` path or `recoveryPending=1` state appeared. Device/output formats in both logs still resolved as `bits=32 validBits=32`, so Windows bit-depth changes are currently represented as active output notifications/rebuilds rather than distinct 16/24-bit application render formats. The stronger signal is `activeSwitchBoundaryPopCandidate candidate=1` with a non-zero old rendered tail dropping into a new entry bridge whose first sample starts at zero, for example previous tail peak `0.2185` before the 88.2 -> 96 kHz bridge. Next playback change should target ActiveSwitchRebuild boundary continuity from the previous rendered tail into the entry bridge, not another round of sample-rate threshold or silence/fade-duration tuning.
- 2026-04-27 shared ActiveSwitchRebuild boundary follow-up: the manual retest also showed artifacts on 44.1/48 kHz and same-rate output-format notifications, so the active-switch startup policy is no longer sample-rate split. All `ActiveSwitchRebuild` conservative rebuilds now log `activeSwitchBoundaryPolicy name=shared-rebuild` and use the same boundary values: `startupSilenceMs=12`, `pcmFadeInMs=48`, and `firstBlockMaxFadeGain=0.35`. The entry bridge is also analyzed as the real first submitted PCM block so the next render block is no longer treated as the first post-configure block in artifact state. Validation: `scripts/build-app.ps1 -BuildDir build-mm -Configuration Debug` passed; `scripts/run-playback-smoke.ps1` with the 48 kHz `SameOutputInvalidationIoError` trigger on `build-mm/fixtures/ab-pink-noise-48k-stereo-minus18db.wav` passed 10/10 conservative rebuilds, 10/10 first data blocks, 10/10 first-block entry guards, and 10/10 boundary envelopes; `scripts/analyze-audio-artifacts.ps1` reported `audioArtifact total:0` for `player-smoke-20260427-030254-775-18dcaeb9.log`.
- 2026-04-27 active-switch preflight and confirmed fade follow-up: the 12 ms shared boundary tuning was treated as the wrong direction and reverted to the 48 kHz baseline (`startupSilenceMs=8`, `pcmFadeInMs=32`, `firstBlockMaxFadeGain=0.50`). Active switching now has an explicit `Preflight` phase and `activeSwitchPreflight` logs that record the output decision (`device-unchanged`, `hot-reconfigure`, `conservative-rebuild`, or `paused-rearm`) with device/format/buffer/fresh-buffer context. Confirmed active switches that still have an active old output now log `activeSwitchPreFade` and run the short output fade before output suspend, hot reconfigure, or conservative rebuild; already-invalidated same-output paths do not claim a fade. `scripts/analyze-audio-artifacts.ps1` now reports recent preflight decisions and pre-fades, and `scripts/run-playback-smoke.ps1` can assert their counts. Validation: Debug build passed. The 48 kHz `SameOutputInvalidationIoError` smoke passed 10/10 conservative rebuilds and 10/10 preflight decisions with zero pre-fades, as expected for an already-stopped old output, and `audioArtifact total:0` for `player-smoke-20260427-031856-672-83a47849.log`. A normal forced output refresh smoke passed with one `activeSwitchPreFade`, one preflight decision, hot reconfigure, and `audioArtifact total:0` for `player-smoke-20260427-031932-285-b3d20458.log`.
- 2026-04-27 post-invalidation restore correction attempt: debug manual log `player-20260427-032824-322-49624.log` showed spatial-audio restore still going through `OutputFormatChange -> WaitingForInvalidation -> conservative-rebuild`, with `activeSwitchPreFade` absent because the old WASAPI session had already reported `IOError` before the app could fade it. A temporary `post-invalidation-rebuild` policy tried longer 12 ms startup timing, but audible testing showed timing alone is not the root fix; the current baseline is back to 8 ms while post-invalidation handling remains the active investigation target.
- 2026-04-27 NormalStart playback-pop follow-up: manual log `player-20260427-033442-835-70164.log` showed the reported "load and play" pop is not an active switch at all; it is the initial `NormalStart` path, which previously had artifact tracking disabled. `NormalStart` now enables startup artifact tracking (`artifactPath=NormalStart`, `recentControlEvent=NormalStart:play`) and starts muted for deferred stream fade-in without injecting startup silence or discarding warmup audio. `scripts/analyze-audio-artifacts.ps1` now classifies `normal-start-boundary` separately from active-switch and recovery paths.
- 2026-04-27 ActiveSwitchRebuild validation correction: the attempted previous-tail continuity bridge was rejected after audible testing. It made the internal PCM boundary look continuous, but a post-invalidation WASAPI path can already be physically silent or interrupted at the endpoint, so starting the new session from the old tail can create a real zero-to-nonzero step that the log-only artifact monitor does not hear. Post-invalidation rebuild timing remains on the 8 ms baseline, and entry bridge PCM now stays zero-anchored through the normal fade-in instead of forcing old-tail continuity. Existing smoke/analyzer output is now treated as internal correctness evidence only, not acoustic pass/fail.
- 2026-04-27 diagnostics-only follow-up: before attempting more pop/click fixes, the app now keeps readable `PlayerLogger` text logs and also writes per-run JSONL diagnostics next to the text log, including generic log events plus structured active-switch, recovery, sink/backend-error, starvation, and internal PCM glitch-monitor events. CLI and smoke automation gained output-device listing/selection/switch options and JSON report generation. Reports use `PASS` / `FAIL` / `INCONCLUSIVE`; clean playback without actual output capture reports pop/click verification as `INCONCLUSIVE`, and internal PCM glitch-monitor evidence remains explicitly below the speaker/headphone output layer.
- 2026-04-28 external spatial-audio validation: on the same 146K server machine, toggling Windows spatial audio causes a small baseline audible pop/click across unrelated apps/games, including a UE5/Vulkan game and a Unity/DX11 game. AudioPlayer may still amplify that baseline during active output reconfiguration, and the target is not absolute zero pop or dismissing the issue as only Windows/system behavior. The remaining spatial-audio switch pop is not a player-specific release blocker only if AudioPlayer's pop is comparable to the other-app baseline. Continue treating large pop, playback reset, silence/no-sound, crash, or failure to recover as AudioPlayer bugs. Keep the diagnostic/reporting layer because it remains useful for future regressions, and future playback changes should avoid overfitting to Windows spatial-audio UI toggling unless logs show a clear player-side fault.
- 2026-04-28 render-mirror and stdout-drain follow-up: the WASAPI render worker now captures short pre/post raw PCM windows around ActiveSwitchRebuild/ErrorRecovery to `*-render-mirror-session*-pre.raw` and `*-post.raw` with JSON metadata, and analyzes the actual PCM submitted to `IAudioRenderClient` separately from decoder/internal PCM. Smoke reports now distinguish decoder/internal artifact candidates, WASAPI submitted PCM artifacts, and clean submitted PCM with an output transition. `FfmpegDecoderWorker` now defaults to `readyReadStandardOutput` direct stdout drain into `PcmStreamBuffer`; the old stdout capture-file path remains available via `AUDIOPLAYER_FFMPEG_STDOUT_CAPTURE=1`. A one-shot 10 ms backpressure retry is used only while QProcess already has pending stdout and the PCM buffer is full. Active output switches now default to fresh-buffer conservative rebuild rather than hot reconfigure.
- Validation: `scripts/build-app.ps1 -BuildDir build-mm -Configuration Debug` passed with the existing `VCINSTALLDIR` deploy warning. `scripts/run-playback-smoke.ps1` with two `SameOutputInvalidationIoError` switches on `build-mm/fixtures/ab-pink-noise-48k-stereo-minus18db.wav` produced `reportResult:WARN`, `renderMirrorClean:True`, no underrun, no stall/lag, and `systemInvalidationDuringSwitch:True` for `player-smoke-20260428-010215-893-7c095499.log`. A normal forced output refresh smoke produced `reportResult:PASS`, `renderMirrorClean:True`, no underrun, no stall/lag, and `systemInvalidationDuringSwitch:False` for `player-smoke-20260428-010237-624-f7fcde06.log`. These PASS/WARN results only prove the submitted PCM layer; manual listening or WASAPI loopback remains required for actual speaker/headphone output transients.
- 2026-04-28 source-switch contamination fix: `PcmStreamBuffer` now has owner session/generation metadata. Decoder appends use owner-checked writes, WASAPI output reads use owner-checked reads, and stale writes/reads emit `stale_session_write` / `stale_buffer_read` diagnostics that fail smoke reports. `FfmpegDecoderWorker` readyRead/finished/error callbacks now capture the expected session and process; stale callbacks discard pending stdout instead of appending, and stop/cleanup disconnect process signals and discard pending stdout before dropping the process. Source/startup teardown now quarantines and clears the old buffer with discard-writes/end-of-stream before releasing output, then stops the decoder and deletes the buffer. Output release logs old endpoint padding that remains after the muted drain budget as `old_endpoint_padding_abandoned`.
- Validation: Debug build passed with the existing `VCINSTALLDIR` deploy warning. Plain pink-noise smoke passed with submitted PCM mirror clean (`player-smoke-20260428-013041-513-bd82bc11.log`). Forced output refresh remained PASS (`player-smoke-20260428-013055-218-3e941fb8.log`). Two `SameOutputInvalidationIoError` switches remained WARN because system invalidation was involved, with no stale writes/reads, no underrun, and no stall/lag (`player-smoke-20260428-013112-981-4d7cfb36.log`). Source-switch contamination smoke from pink noise to `smoke-alac.m4a` passed with `oldPcmLeakDetected:False`, `sourceSwitchMirrorClean:True`, `sourceSwitchClean:True`, `staleSessionWriteDetected:False`, and `staleBufferReadDetected:False` (`player-smoke-20260428-013022-151-9de6cd00.log`). Submitted PCM checks still do not replace manual listening or WASAPI loopback for actual endpoint output.
- 2026-04-28 spatial endpoint residual direction: manual reproduction showed old audio can still be heard on cold start and source switch only when Windows spatial audio / Dolby-style endpoint processing is enabled, even though submitted PCM/session guards report clean. Treat `oldPcmLeakDetected:False`, `sourceSwitchClean:True`, and render-mirror clean as submitted-PCM-layer evidence only. Reports now keep `popClickVerification=INCONCLUSIVE` without loopback/manual output evidence, add `actualEndpointOutputVerification=INCONCLUSIVE`, `leakLayer=unknown` when loopback is absent, and expose `sourceSwitchSubmittedPcmClean` plus `sourceSwitchCleanLayer=submitted-pcm`.
- Minimal mitigation now supports manual opt-in endpoint flushing with `AUDIOPLAYER_SPATIAL_ENDPOINT_FLUSH=1`, `AUDIOPLAYER_SPATIAL_ENDPOINT_FLUSH_MS` (default 200), and `AUDIOPLAYER_SPATIAL_ENDPOINT_SETTLE_MS` (default 150). The flush opens the target WASAPI shared render endpoint, sets the flush stream to zero gain when available, submits silent frames, starts briefly, then stops/resets and settles before real playback. To avoid render-worker starvation, it runs after the old output has been released and before the new real playback session starts; this covers cold start, source-switch restart, and conservative output-refresh/active-switch rebuild startup without mixing the flush into active-switch or recovery state.
- Startup submitted mirrors now capture 1000 ms for cold start/source start/active rebuild/error recovery and include `sourcePath`, `previousSourcePath`, `sessionId`, `bufferGeneration`, `selectedOutputDeviceId`, `outputFormat`, `startupObservationProfile`, and `appStartTime`. The last 500 ms of submitted PCM is saved as a lightweight previous-run tail fingerprint JSON next to the logs; loopback capture and previous-tail similarity comparison are still pending, so `previousRunAudioLeakAtColdStart` remains false/unknown rather than asserted.
- Validation: Debug build passed with the existing `VCINSTALLDIR` deploy warning. Plain cold-start smoke passed with `popClickVerification:INCONCLUSIVE` and `coldStartSubmittedMirrorClean:True` (`player-smoke-20260428-015117-136-b62f68cb.log`). Source-switch contamination remained PASS with submitted mirror clean (`player-smoke-20260428-015131-056-5c7ae02e.log`). Forced output refresh remained PASS (`player-smoke-20260428-015145-567-a0f02c2d.log`). Two-switch `SameOutputInvalidationIoError` remained WARN only because system invalidation occurred (`player-smoke-20260428-015145-599-81e564ee.log`). With `AUDIOPLAYER_SPATIAL_ENDPOINT_FLUSH=1`, cold start passed with one flush (`player-smoke-20260428-015159-529-35a396db.log`), source switch passed with two flushes and no stale PCM (`player-smoke-20260428-015404-416-7a027954.log`), forced refresh passed with two flushes and no stall/lag (`player-smoke-20260428-015323-752-e14c7fa8.log`), and `SameOutputInvalidationIoError` remained WARN only for expected system invalidation with no stall/lag (`player-smoke-20260428-015342-589-3ba44ca5.log`). Actual spatial-audio residual suppression still requires user-side manual listening or a future WASAPI loopback capture.
- 2026-04-28 loopback capture first pass: endpoint-layer evidence now has a
  separate sidecar path through the standalone `WasapiLoopbackCapture` console
  tool and `scripts/run-loopback-manual-smoke.ps1`. This records the default
  render endpoint via WASAPI loopback to WAV plus JSON, then runs the normal
  smoke harness. It does not change playback logic, WASAPI switching behavior,
  UI, app reports, or regression defaults. First-pass detection only flags
  obvious peak, clipping, sudden sample-delta, and short peak-to-rms transient
  candidates; it is useful for validating manual pop/click observations but is
  not a complete acoustic classifier.
- Spatial Audio switching can invalidate the loopback capture client itself
  (`AUDCLNT_E_DEVICE_INVALIDATED`, observed as `0x88890004`). Treat this first
  as capture-side endpoint invalidation: preserve partial WAV/report evidence
  with `result=INCONCLUSIVE`, `captureInterrupted=true`, and an interruption
  HRESULT before interpreting the loopback evidence or making playback changes.
- Off -> Windows Sonic manual smoke has invalidated the loopback capture near
  the switch/recovery window, so single-client loopback can miss recovery-side
  endpoint audio. The next loopback evidence should use multi-segment capture:
  close the invalidated client, reopen the default render endpoint, keep
  per-segment WAV/report metadata, and still treat detector silence as
  inconclusive rather than proof that the endpoint was pop-free.
- Pink-noise loopback is a weak detector input for light transients because the
  source itself raises local peak/RMS activity. For future Off -> Windows Sonic
  loopback checks, use generated silence or low-level tone fixtures first, then
  compare with pink-noise subjective continuity only as supporting context.
- 2026-04-29 silence-fixture manual retest note: Windows Sonic -> Off with
  `silence-48k-stereo.wav` was manually heard as NoPop and the app-side
  diagnostics were clean (`maxAudioPeak=0`, no submitted PCM discontinuity,
  no internal glitch candidates, render mirror clean, and no underrun, stale
  data, or recovery). A prior wrapper invocation accidentally passed
  `ManualEndpointResult=PopHeard`; treat that parameter value as operator input
  error, not as evidence for the conclusion.
- Harness and loopback artifacts now use grouped retention for generated
  `player-smoke-*`, `player-loopback-smoke-*`, and `loopback-*` files. The
  default keeps the newest 20 run groups across `build-mm/cache/logs` and
  `build-mm/cache/loopback`; scripts expose `-KeepRuns` and `-NoCleanup` for
  manual evidence preservation.
- 2026-04-29 multi-segment loopback follow-up: `WasapiLoopbackCapture` now
  treats endpoint invalidation as a segment boundary. On
  `AUDCLNT_E_DEVICE_INVALIDATED` / `0x88890004` and related WASAPI invalidation
  HRESULTs, it finalizes the current segment WAV and per-segment JSON metadata,
  reopens the current default render endpoint, and continues capture until the
  requested window or stop file ends. The aggregate sidecar report lists segment
  WAV paths, segment metadata paths, start/end timestamps, per-segment
  interruption HRESULTs, per-segment transient counts, and aggregate transient
  counts. Detector silence now reports `INCONCLUSIVE`; endpoint loopback silence
  is evidence to inspect, not proof that Windows Sonic, Dolby Atmos, endpoint
  APO, driver, or physical output were pop/click free. Loopback evidence remains
  sidecar-only and is not merged into the app JSON report.
- 2026-04-29 harness-exit and release-diagnostics follow-up: the smoke wrapper
  now records the launched app PID, waits for the app using the configured
  quit/timeout budget, and performs a post-exit descendant process cleanup so
  lingering `ffmpeg.exe` or other child processes do not keep wrapper flows
  unreliable. Timeout handling records descendants before cleanup, killed PIDs,
  cleanup errors, and residual PIDs in the harness report; the regression
  aggregate preserves each case's process-cleanup summary. App-side quit logs
  now bracket scheduled quit, fired quit, and `aboutToQuit` report writing.
  WASAPI release diagnostics now log release padding/buffer state, stream fade
  begin/end, render callbacks during stop fade, event-notifier disable points,
  drain-before-reset begin/done/timeout, and Stop/Reset padding. These are
  diagnostic-only changes and do not change playback core behavior. Artifact
  monitor and render mirror evidence remains submitted-PCM-layer evidence only:
  clean results cannot prove Windows Sonic, Dolby Atmos, endpoint APO, driver, or
  physical endpoint output are pop/click free.
