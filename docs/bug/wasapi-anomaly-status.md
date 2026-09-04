# Windows WASAPI anomaly status

This file holds stage-specific notes for the current Windows WASAPI playback
anomaly investigation. Keep durable cross-task rules in `AGENTS.md` and
`docs/dev/*.md`; keep temporary investigation state here.

## Durable guidance

- High-level agent entry point: `AGENTS.md`
- Bug/status tracking index: `docs/bug/README.md`
- Workflow and code-change scope: `docs/dev/agent-workflow.md`
- Harness and smoke-test policy: `docs/dev/harness.md`
- Diagnostics and evidence layers: `docs/dev/diagnostics.md`
- WASAPI switching principles: `docs/dev/wasapi-switching.md`
- Git and release workflow: `docs/dev/release-workflow.md`

## Status refresh: 2026-06-30 (libav multi-channel image and startup-rate check)

- **Symptom under review**: User manual testing reported that libav playback
  can feel slightly fast at the beginning, and multi-channel output on the
  Sound BlasterX G6 images right with normal channel order but left when forced
  Creative channel reorder is enabled.
- **Follow-up manual result**: After the libav post-SWR Creative reorder fix,
  user testing reported startup speed as stable, but multi-channel output still
  felt right-shifted, and the `.mlp` version did not image like the `.eb3`
  version.
- **Local media coverage update (2026-06-30)**: User-provided files under
  `media/` now cover the current Dolby/layout investigation set:
  `POWDER SNOW Live V9.8.6.eb3` (`eac3`, Atmos, 8 ch, `5.1.2`),
  `POWDER SNOW Live V9.8.6.mlp` (`truehd`, Atmos, 8 ch, `7.1`),
  plus high-resolution stereo FLAC (`352800 Hz`) and ALAC/M4A (`96000 Hz`).
  The set is sufficient for the EB3-vs-MLP spatial/layout comparison and
  stereo regression checks, but it still lacks a plain non-Atmos 5.1 control
  sample.
- **Decoder layout preservation change (2026-06-30)**:
  `PcmStreamFormat` now carries an optional FFmpeg channel-layout string,
  `LibavSeekDecoderWorker` initializes/reconfigures SWR with that explicit
  layout when present, and the PCM seek-cache key includes the layout so 8ch
  `5.1.2` and 8ch `7.1` decoded PCM cannot be reused across each other.
  WASAPI currently infers `eac3`/8ch as `5.1.2` and `truehd`/8ch as `7.1`.
- **Validation (2026-06-30)**:
  `scripts\run-playback-smoke.ps1 -Source "media\POWDER SNOW Live V9.8.6.eb3" -BuildDir build-mm -Configuration Debug -QuitAfterMs 6000 -RequirePlaying -RejectPlaybackErrors`
  passed with report
  `build-mm/cache/logs/player-smoke-20260630-225334-615-ba10acaf.report.json`.
  `scripts\run-playback-smoke.ps1 -Source "media\POWDER SNOW Live V9.8.6.mlp" -BuildDir build-mm -Configuration Debug -QuitAfterMs 6000 -RequirePlaying -RejectPlaybackErrors`
  passed with report
  `build-mm/cache/logs/player-smoke-20260630-225351-919-8d7fdd96.report.json`.
  Both runs selected WASAPI shared stereo on the current G6 endpoint, so they
  validate normal playback/downmix startup, not native multi-channel WASAPI
  rendering.
- **New limitation/evidence (2026-06-30)**: The current Sound BlasterX G6
  WASAPI endpoint reported `preferredChannels=2` and selected
  `384000 Hz`/2ch in both shared and exclusive smoke runs. An attempted
  exclusive EB3 run selected `384000 Hz`/2ch/24-bit and failed submitted-PCM
  artifact checks:
  `build-mm/cache/logs/player-smoke-20260630-225302-739-80cb2e62.report.json`.
  This reinforces that the next native spatial route should use
  `ISpatialAudioClient` static bed rather than relying on the normal WASAPI
  mix/exclusive endpoint to expose 5.1.2.
- **Follow-up evidence**:
  `build-mm/playable/Debug/20260630-140326/cache/logs/player-20260630-144622-578-47376.log`.
  `ffprobe` identified the `.eb3`/EAC3 Atmos source and sidecar as
  `channel_layout=5.1.2`, while the `.mlp`/TrueHD Atmos source and sidecar are
  `channel_layout=7.1`. The Sound BlasterX G6 output mask in the same run was
  `0x0000063f`, which maps to flat `7.1` (`FL+FR+FC+LFE+BL+BR+SL+SR`).
  FFmpeg layout metadata for `5.1.2` is
  `FL+FR+FC+LFE+SL+SR+TFL+TFR`.
- **Follow-up analysis**: Direct FFmpeg decode of the sidecars to flat `7.1`
  and the app render-mirror submitted PCM showed the same left/right energy
  deltas in the first one-second exclusive-mode window. That narrows the
  perceived opening right-shift to decoded/layout-converted PCM before WASAPI
  submission, not an additional WASAPI packing or endpoint-write swap. Longer
  20-second windows at 15 s, 60 s, and 120 s were left-weighted or near
  balanced, so the evidence does not show a sustained whole-track right bias.
- **Current interpretation**: EB3/EAC3 and MLP/TrueHD are not equivalent
  multi-channel inputs here. EB3 is a `5.1.2` height layout; converting it to
  the G6 flat `7.1` target leaves `BL/BR` effectively silent in the measured
  FFmpeg/app output, while MLP/TrueHD `7.1` carries real `BL/BR` content. The
  EB3-vs-MLP image difference is therefore expected unless the app adds an
  explicit height-to-flat fold/remap policy or routes through an endpoint
  spatial renderer. No such policy was implemented in this pass.
- **Spatial route decision**: The next implementation direction is the Windows
  `ISpatialAudioClient` static PCM bed path, not more flat-7.1 channel reorder
  tuning.
- **Spatial static-bed implementation (2026-06-30)**: EAC3/8ch sources now opt
  into an experimental `ISpatialAudioClient` static bed path by default, unless
  `AUDIOPLAYER_WASAPI_SPATIAL_STATIC_BED=0` disables it. The player requests
  decoder PCM as 48 kHz/8ch/float32 `5.1.2`, activates an
  `ISpatialAudioObjectRenderStream` with static objects
  `FL,FR,FC,LFE,SL,SR,TFL,TFR`, and deinterleaves the interleaved PCM into
  mono object buffers for the Windows spatial engine. Rendering is driven only
  by the spatial stream event; an earlier local attempt also rendered from the
  PCM buffer `readyRead` signal and reproduced `SPTLAUDCLNT_E_INTERNAL`
  (`0x8889010d`) on the second `BeginUpdatingAudioObjects` call.
- **Spatial static-bed validation (2026-06-30)**:
  `scripts\build-app.ps1 -BuildDir build-mm -Configuration Debug` - PASS,
  generated playable bundle
  `build-mm/playable/Debug/20260630-231956/AudioPlayer.exe`.
  `scripts\run-playback-smoke.ps1 -Source "media\POWDER SNOW Live V9.8.6.eb3" -BuildDir build-mm -Configuration Debug -QuitAfterMs 8000 -RequirePlaying -RejectPlaybackErrors -RequireLogPattern "spatialStaticBed configured"`
  - PASS, report
  `build-mm/cache/logs/player-smoke-20260630-232004-733-74d6a795.report.json`;
  the log selected `5.1.2`, configured the static bed, activated 8 objects, and
  did not enter WASAPI error recovery.
  `scripts\run-playback-smoke.ps1 -Source "media\POWDER SNOW Live V9.8.6.mlp" -BuildDir build-mm -Configuration Debug -QuitAfterMs 8000 -RequirePlaying -RejectPlaybackErrors`
  - PASS, report
  `build-mm/cache/logs/player-smoke-20260630-232024-473-c4be141b.report.json`;
  MLP/TrueHD remained on the existing non-spatial stereo path
  (`spatialStaticBedRequested=0`).
  Unit tests passed with
  `scripts\run-tests.ps1 -BuildDir build-mm -Configuration Debug -NoBuild`,
  report `build-mm/test-report.json`. Harness report schema validation passed
  with `scripts\test-harness-reports.ps1`.
- **Evidence limit**: This proves the app can configure and feed the Windows
  spatial static-bed path on the current Sound BlasterX G6 endpoint without
  scripted playback errors. It is still not an endpoint-output or manual
  listening claim about actual binaural image centering, Dolby Access behavior,
  or pop/click-free physical output.
- **Spatial probe implementation**: `selectOutputFormat()` now probes
  `ISpatialAudioClient` on the selected WASAPI endpoint and logs
  `spatialAudioProbe ...` capability lines, including native static object
  mask, `supports5.1.2`, dynamic object count, static object positions, and
  supported per-object audio formats. Smoke reports now expose these fields as
  `spatialAudioProbeObserved`, `spatialAudioStreamAvailable`,
  `spatialAudioSupportsFiveOneTwo`, `spatialAudioNativeMask`,
  `spatialAudioMaxDynamicObjects`, `spatialAudioObjectFormatCount`, and
  `spatialAudioFirstObjectFormat`.
- **Spatial probe validation**:
  `scripts\run-playback-smoke.ps1 -Source "build-mm\fixtures\smoke.wav" -BuildDir build-mm -Configuration Debug -QuitAfterMs 2500 -RequirePlaying -RejectPlaybackErrors`
  - PASS, report
  `build-mm/cache/logs/player-smoke-20260630-223635-592-2d070aa2.report.json`.
  On the Sound BlasterX G6, `ISpatialAudioObjectRenderStream` was available,
  `spatialAudioSupportsFiveOneTwo=true`, the native mask included
  `FL,FR,FC,LFE,SL,SR,BL,BR,TFL,TFR,...`, and the supported object format was
  `rate=48000 channels=1 bits=32 subtype=float`.
- **Next implementation slice**: Add an experimental spatial render worker/path
  that takes known PCM layouts such as `5.1.2`, deinterleaves to mono float32
  object buffers, and writes static objects (`FL/FR/FC/LFE/SL/SR/TFL/TFR`) via
  `ISpatialAudioObjectRenderStream`. Keep it opt-in until endpoint listening or
  loopback evidence shows that EB3 imaging improves.
- **Log evidence inspected**:
  `build-mm/playable/Release/20260630-020909/cache/logs/player-20260630-022637-204-88648.log`.
  The 2026-06-30 run used libav-inprocess for ALAC and EAC3 sidecar playback.
  Render-rate logs matched expected frame counts after the initial pre-render
  window (for example 384 kHz stereo and 96 kHz/48 kHz 8-channel sessions
  repeatedly logged submitted frames equal or very close to expected frames).
  This does not support a sustained actual fast-playback clock bug from the log
  layer alone.
- **Finding**: The libav Creative reorder path used `swr_set_matrix()` during
  SWR setup, while the FFmpeg CLI path applies `pan=` after `aresample` and
  `aformat`. Those are not equivalent whenever SWR also changes channel count
  or layout; the libav path could reorder while converting instead of reordering
  the final target-layout PCM. This makes forced reorder a plausible cause of
  the reported opposite-side image shift.
- **Fix**: `LibavSeekDecoderWorker` now applies Creative channel reorder after
  `swr_convert()` on the final 6/8-channel PCM block and logs
  `libavSeek creativeChannelReorder postSWR ...` once per decode state. The
  SWR setup-time matrix was removed so libav matches the CLI path semantics.
- **Validation**:
  - Unit tests: `scripts\run-tests.ps1` - PASS, all 10 suites, report
    `build-mm/test-report.json`.
  - App build: `scripts\build-app.ps1 -BuildDir build-mm -Configuration Debug` -
    PASS, generated playable bundle
    `build-mm/playable/Debug/20260630-140326/AudioPlayer.exe`.
- **Evidence limits**: This is a submitted-PCM/channel-order fix. It does not
  prove physical endpoint image centering or pop/click-free output; a follow-up
  G6 listening or loopback run is still required.





## Status refresh: 2026-06-06 (WAV quit/close stop fade)

- **Symptom**: WAV playback did not sound fully settled when closing the app
  window / quitting during playback.
- **Finding**: The close/quit path already calls `stopPlayback()` before
  teardown, and WAV quit smoke showed a completed stop PCM fade, but the fade
  duration was only 32 ms (1536 frames at 48 kHz), which can still sound abrupt.
- **Fix**: Increased `kPcmFadeOutDurationMs` from 32 ms to 80 ms, so explicit
  stop, app quit, and close-window paths submit a longer sample-level WASAPI
  fade-out before releasing output resources.
- **Validation**:
  - Build: `./scripts/build-app.ps1 -BuildDir build-mm -Configuration Debug` -
    PASS.
  - WASAPI shared WAV quit:
    `./scripts/run-playback-smoke.ps1 -Source "build-mm\fixtures\smoke.wav" -BuildDir build-mm -Configuration Debug -QuitAfterMs 4000 -RequirePlaying -RejectPlaybackErrors -RequireStopFadeOut` -
    PASS, `stopFadeOutSubmittedFrames=3840`, `stopFadeOutFinalGain=0`,
    report
    `E:\AI\OpenCode\AudioPlayer\build-mm\cache\logs\player-smoke-20260606-011427-706-6e1d2d72.report.json`.
  - WASAPI exclusive WAV quit:
    `./scripts/run-playback-smoke.ps1 -Source "build-mm\fixtures\smoke.wav" -BuildDir build-mm -Configuration Debug -QuitAfterMs 4000 -RequirePlaying -RejectPlaybackErrors -RequireStopFadeOut -ExclusiveMode` -
    PASS, `stopFadeOutSubmittedFrames=3840`, `stopFadeOutFinalGain=0`,
    report
    `E:\AI\OpenCode\AudioPlayer\build-mm\cache\logs\player-smoke-20260606-011441-614-584e65b3.report.json`.
- **Evidence limits**: Submitted-PCM/render-layer validation only; no manual
  listening or loopback endpoint capture was performed in this pass.

## Status refresh: 2026-06-05 (WASAPI exclusive exact playback from ASIO)

- **Bug**: Switching from ASIO to WASAPI exclusive mode caused channel count to
  drop to 2 for multi-channel MLP/TrueHD files, even though shared→exclusive
  preserved 8 channels.
- **Root cause**: `MainWindow::replacePlayer()` (mainwindow.cpp:548) set
  `m_exactPlaybackEnabled = isAsio && stored`. When creating a WASAPI player,
  `isAsio = false`, so exact playback was forced off. This made
  `selectOutputFormat` prefer the 2-channel mix format over the 8-channel source
  in the exclusive candidate list.
- **Fix**: Removed `isAsio &&` from `replacePlayer` so WASAPI exclusive also
  respects the stored exact playback preference (default `true`).
- **Validation**:
  - Build: scripts\build-app-msvc.cmd -BuildDir build-mm — PASS
  - Regression: scripts\run-playback-regression.ps1 -CaseFilter wav-play-stop — PASS
  - Harness self-test: scripts\test-harness-reports.ps1 -SelfTest — PASS
- **Impact**: WASAPI exclusive mode now uses exact playback by default, matching
  source channel count (e.g., 8ch for MLP/TrueHD) instead of falling back to
  mix format (typically 2ch). No impact on shared mode or ASIO.
- **Files changed**: `mainwindow.cpp`

## Status refresh: 2026-06-04 (DolbyDownmix EAC3/AC3 fix)

- **Bug**: libavseekdecoderworker.cpp line 336 set swrOutputChannelCount to
  input channel count (6) instead of output channel count (2), causing
  DolbyDownmixProcessor to read uninitialized memory for 5.1→stereo conversion.
- **Root cause**: SWR was configured to output 2 channels, but bookkeeping
  field said 6. DolbyDownmix condition 6 != 2 was always true, causing it to
  read 6-channel interleaved data from a 2-channel buffer.
- **Fix (Option B)**: Two changes in libavseekdecoderworker.cpp:
  1. Line 336: Changed swrOutputChannelCount to outputFormat.channelCount
  2. After line 601: Added SWR reconfiguration block that sets output channels
     to input channels when DolbyDownmix is active, preserving Dolby LtRt/DPLII
     coefficients (cmix=0.595, smix=0.500)
- **Validation**:
  - Build: scripts\build-app-msvc.cmd -BuildDir build-mm -Configuration Debug — PASS
  - EAC3: un-playback-smoke.ps1 -Source smoke.ec3 — PASS (peak=0.302, clean)
  - AC3: un-playback-smoke.ps1 -Source smoke.ac3 — PASS (peak=0.214, clean)
  - Regression: un-playback-regression.ps1 -CaseFilter wav-play-stop — PASS
- **Impact**: Fixes Dolby Atmos EAC3/AC3 5.1→stereo playback. No impact on
  stereo files or non-Dolby multi-channel content.
- **Branch**: opencode-0528 (current working branch).
## Status refresh: 2026-06-04 (Phase 3: WASAPI worker header under 1000 lines)

- **Structure split**: Moved 30 method implementations from
  windowswasiaudioplayer_worker.h (2419 lines) to
  windowswasiaudioplayer_worker.cpp (2904 lines).
- **Worker header reduced**: windowswasiaudioplayer_worker.h now **760 lines**
  (down from 2419 after Phase 2, target was <1000).
- **Key methods moved**:
  - configureOutput (363 lines) — setup path
  - enderAvailableFrames (480 lines) — render callback (invoked by Qt event loop)
  - eleaseOutput (185 lines) — teardown path
  - handleFatalError (54 lines) — error path
  - Plus 26 smaller methods (pause/resume, fade, volume, diagnostics, etc.)
- **Kept inline in header** (render hot path):
  - setStreamGain, adeStreamGainTo, pplyGainToSample
  - eadNormalizedSample, currentFadeEndpointGain
  - pplyPcmFadeIn, pplyOutputVolume
  - pplyFadeVolumeAndConvert, copyConvertedFramesToRenderBuffer
  - rtifactTrackingEnabled, emitIdleIfDrained
- **Validation**:
  - Build: scripts\build-app-msvc.cmd -BuildDir build-mm -Configuration Debug — PASS
  - Regression: scripts\run-playback-regression.ps1 -BuildDir build-mm -Configuration Debug -CaseFilter wav-play-stop -NoCleanup — PASS
  - Harness self-test: scripts\test-harness-reports.ps1 -SelfTest — PASS
- **Evidence limits**: Submitted-PCM-clean only; no endpoint acoustic verification.
- **Branch**: opencode-0528 (current working branch).
## Status refresh: 2026-06-04 (shared backend common module)

- **Shared module created**: src/backends/shared/audioutils.h
  - channelLayoutForCount() — channel count to layout string mapping
  - pcmCodecName() / pcmSampleFormatName() / pcmMuxerName() — PCM format names
- **Backends updated**:
  - windowswasiaudioplayer_state.cpp — delegates to AudioUtils::
  - fmpegaudioplayer_state.cpp — delegates via 	oPcmEncoding() converter
  - windowsasioaudioplayer_formats.cpp — delegates via 	oPcmEncoding() converter
- **Validation**:
  - Build: scripts\build-app-msvc.cmd -BuildDir build-mm -Configuration Debug — PASS
  - Regression: scripts\run-playback-regression.ps1 -BuildDir build-mm -Configuration Debug -CaseFilter wav-play-stop -NoCleanup — PASS
  - Harness self-test: scripts\test-harness-reports.ps1 -SelfTest — PASS
- **Evidence limits**: Submitted-PCM-clean only; no endpoint acoustic verification.
- **Branch**: opencode-0528 (current working branch).
## Status refresh: 2026-06-04 (Phase 2: extracted WASAPI worker diagnostics)

- **Structure split**: Extracted 16 diagnostic methods from
  windowswasiaudioplayer_worker.h (2220 lines) to new file
  windowswasiaudioplayer_worker.cpp (1047 lines).
- **Worker header reduced**: windowswasiaudioplayer_worker.h now 2220 lines
  (down from 3360 after Phase 1, ~1140 lines removed in Phase 2).
- **Extracted methods**:
  - lushSpatialEndpoint (254 lines) - spatial audio endpoint flush
  - submitPcmFadeOutBeforeStop (190 lines) - PCM fade-out before stop
  - drainMutedPaddingBeforeReset (86 lines) - drain padding before reset
  - 
oteFirstSubmittedPcmAfterSeek (74 lines) - seek resume timing
  - logSeekResumeLatencyIfNeeded (43 lines) - seek resume latency logging
  - Render mirror system (357 lines): startRenderMirrorCapture,
    inishRenderMirrorCapture, ppendSubmittedPcmTail,
    captureSeekResumeFirst50msSubmittedPcm, mirrorSubmittedBlock
  - Artifact analysis (188 lines): observeArtifactSilence,
    nalyzeArtifactBlock
  - Diagnostic helpers (58 lines): ormatJson, enderMirrorBasePath,
    previousRunTailFingerprintPath, saveSubmittedTailFingerprint
- **Key fix**: Moved structs used in class method signatures
  (RenderedBlockMetrics, PcmFadeApplication, ActiveSwitchBoundaryPolicy,
  WasapiArtifactTrackingConfig, NoiseShaperState) out of anonymous namespace
  to ensure consistent type identity across translation units.
- **Validation**:
  - Build: scripts\build-app-msvc.cmd -BuildDir build-mm -Configuration Debug — PASS
  - Regression: scripts\run-playback-regression.ps1 -BuildDir build-mm -Configuration Debug -CaseFilter wav-play-stop -NoCleanup — PASS
  - Harness self-test: scripts\test-harness-reports.ps1 -SelfTest — PASS
- **Evidence limits**: Submitted-PCM-clean only; no endpoint acoustic verification.
- **Branch**: opencode-0528 (current working branch).
## Status refresh: 2026-06-04 (Phase 1: extracted WASAPI worker helpers)

- **Structure split**: Extracted anonymous namespace from
  `windowswasapiaudioplayer_worker.h` (3883 lines) to new file
  `windowswasapiaudioplayer_worker_helpers.h` (562 lines).
- **Worker header reduced**: `windowswasapiaudioplayer_worker.h` now 3360 lines
  (down from 3883, ~523 lines removed).
- **Extracted content**:
  - 36 constants (session/buffer timing, fade/gain, output config, diagnostics, env names)
  - Environment parsing functions (`envFlagDisabled`, `boundedEnvInt`, `wasapiLibavDecoderDisabled`, etc.)
  - Channel layout/mask helpers (`channelLayoutForMask`, `channelMaskForCount`, etc.)
  - PCM format helpers (`pcmEncodingName`, `safeRelease`, `buildWaveFormat`, etc.)
  - Noise shaping functions (`NoiseShaperState`, `noiseShapedQuantize32To24`, etc.)
  - Structs (`ActiveSwitchBoundaryPolicy`, `WasapiArtifactTrackingConfig`, `RenderedBlockMetrics`, `PcmFadeApplication`)
- **Validation**:
  - Build: `scripts\build-app-msvc.cmd -BuildDir build-mm -Configuration Debug` — PASS
  - Regression: `scripts\run-playback-regression.ps1 -BuildDir build-mm -Configuration Debug -CaseFilter wav-play-stop -NoCleanup` — PASS
  - Harness self-test: `scripts\test-harness-reports.ps1 -SelfTest` — PASS
- **Evidence limits**: Submitted-PCM-clean only; no endpoint acoustic verification.
- **Branch**: `opencode-0528` (current working branch).

## Status refresh: 2026-06-02 (merged codex stop-fade and 24-bit clipping fixes)

- **Branch merge**: Merged `codex-0601-wasapi-bitdepth` into `opencode-0528`.
  The merge included two codex commits:
  - `716f827`: Fix WASAPI exclusive 24-bit clipping
  - `5b28560`: Fix WASAPI stop fade and strengthen playback validation
- **Changes merged**:
  - WASAPI exclusive 24-bit clipping fix (noise-shaper dither correction)
  - Stop fade improvements (sample-level PCM tail before resource release)
  - Enhanced playback validation harness assertions
  - Loopback dropout detection improvements
- **Conflict resolution**: Resolved two conflicts in
  `windowswasapiaudioplayer_worker.h` for Int32/24 quantization paths.
  Selected `Updated upstream` version using `roundedQuantize32To24()` with
  boundary checks over inline implementation without bounds.
- **Status update**: This merge brings the opencode branch current with
  codex's WASAPI bit-depth and stop-fade validation work.
- **Branch**: `opencode-0528` (current working branch).

## Status refresh: 2026-06-01 (normal-stop PCM fade)

- User follow-up: 16-bit WASAPI smoke playback still had audible repeated
  artifacts, and closing the application could end with a small pop.
- Normal explicit WASAPI stop now submits a 32 ms sample-level descending PCM
  tail before resource release. The existing stream-volume fade remains a
  secondary safeguard. Output-switch and recovery release paths keep their
  previous lifecycle semantics and do not consume this tail.
- Microsoft documents that session/stream volume interfaces, including
  `IAudioStreamVolume`, do not work with exclusive-mode streams:
  https://learn.microsoft.com/en-us/windows/win32/coreaudio/endpoint-volume-controls.
  Therefore the sample-level PCM tail is required for exclusive stop; the
  existing stream-volume fade is not treated as exclusive-mode proof.
- App automation now stops playback before `QCoreApplication::quit()`, and
  `MainWindow::closeEvent` stops playback before child/backend teardown. This
  places stop-fade logs before the `aboutToQuit` report boundary.
- Validation PASS: G5 WASAPI exclusive 16-bit tone smoke:
  `build-codex-validation-ffmpeg811\cache\logs\player-smoke-20260601-122341-667-e39c63a5.harness.json`.
  It recorded exact 16-bit matching, 1536 submitted fade frames at 48 kHz,
  `stopFadeOutCompleted=true`, and `stopFadeOutLastSubmittedSample=0`.
- Validation PASS: actual window-close ordering with exclusive 16-bit tone:
  `build-codex-validation-ffmpeg811\cache\logs\player-window-close-smoke-final.log`.
  `closeEvent stopPlayback` preceded `stopPcmFadeOut begin/end`, and the
  completed fade preceded `aboutToQuit begin`.
- Endpoint evidence remains limited. A synchronized shared tone loopback run
  reported zero transient and dropout candidates plus a fade-to-silence
  candidate:
  `build-codex-validation-ffmpeg811\cache\logs\player-loopback-smoke-20260601-122535-335-de3275c0.summary.json`.
  Physical G5/Realtek listening is still required before claiming the audible
  artifacts or close pop fixed.

## Status refresh: 2026-05-28

- Branch-integration follow-up: preserved the restored WASAPI menu-switch path
  from `codex/asio-format-fallback`, but removed output backend/device/mode
  persistence instead of keeping the intermediate `QSettings` reset writes.
  Each fresh app launch constructs the system output backend directly, which is
  WASAPI shared on Windows, with the default output device and both exclusive
  and stability mode disabled. The last-opened directory setting is unchanged.
- Selecting playback-menu `独占模式` or `稳定模式（高缓冲）` while ASIO is active
  still routes through the high-level WASAPI switch helper before applying the
  requested WASAPI mode. No timing/debounce workaround was added.
- Validation: Debug build passed with
  `scripts\build-app-msvc.cmd -BuildDir build-codex-asio-format-fallback
  -Configuration Debug -FfmpegAudioCoreRoot
  D:\AI\Codex\AudioPlayer-codex-0523-asio\build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc`.
  Build log:
  `build-claude-logs\build-app-msvc-056e4eccee4a423ebc91df46f8807806.log`.
- Validation: WASAPI shared smoke passed on the 352.8 kHz fixture:
  `build-codex-asio-format-fallback\cache\logs\player-smoke-20260528-014046-634-2ada0e98.harness.json`.
  This smoke confirms default WASAPI shared playback and submitted-PCM cleanliness
  for that run; it does not manually click the menu or verify actual endpoint
  audio.

## Status refresh: 2026-05-26

- Source-rate fallback follow-up: WASAPI exclusive candidate ordering now uses
  the probed source sample rate as the first candidate and falls back through
  common rates, so unsupported high-rate sources can land on a supported output
  format instead of relying only on the Windows mix-format-first order. The
  active-switch transaction model remains unchanged and hot reconfigure remains
  disabled by default in favor of conservative rebuild.
- Validation: Debug build passed with
  `scripts\build-app-msvc.cmd -BuildDir build-codex-asio-format-fallback
  -Configuration Debug -DeployFfmpegExecutable D:\Tool\ffmpeg\bin\ffmpeg.exe
  -DeployFfprobeExecutable D:\Tool\ffmpeg\bin\ffprobe.exe`. A 352.8 kHz WASAPI
  shared smoke selected the current shared mix format at 96 kHz, with harness
  report
  `build-codex-asio-format-fallback\cache\logs\player-smoke-20260526-021453-333-b5f67a36.harness.json`.
  No manual listening or WASAPI loopback endpoint-output verification was
  collected.
- UI switch follow-up: restored the 0416-era playback-menu path so selecting
  `独占模式` or `稳定模式（高缓冲）` from ASIO first switches the active backend to
  WASAPI, then applies the requested WASAPI mode. This fixes the ASIO no-op path
  where stability could appear checked without a backend switch. Rebuild passed
  at `build-codex-asio-format-fallback\playable\Debug\20260526-022131\AudioPlayer.exe`.
  A standalone WASAPI stable shared smoke passed with harness report
  `build-codex-asio-format-fallback\cache\logs\player-smoke-20260526-022217-987-3b3776f0.harness.json`.
  A previous parallel WASAPI smoke during ASIO playback failed with
  `AUDCLNT_E_DEVICE_IN_USE`, consistent with the ASIO run holding the same G5
  endpoint; it is not treated as a standalone WASAPI regression.
- Startup default follow-up: restored the 0416-era startup policy. Each app
  launch now resets output backend/device/mode settings to WASAPI shared on the
  default output device (`exclusiveMode=0`, `stabilityMode=0`) instead of
  restoring the last selected ASIO device or stability-mode checkbox. Validation:
  build passed at
  `build-codex-asio-format-fallback\playable\Debug\20260526-022811\AudioPlayer.exe`;
  default startup smoke loaded `WASAPI shared`, logged `usesDefault=1`, and
  configured WASAPI with `exclusiveMode=0 stabilityMode=0`. Harness report:
  `build-codex-asio-format-fallback\cache\logs\player-smoke-20260526-022818-128-806cf10e.harness.json`.
- UI label follow-up: media-info backend/decoder labels are now refreshed from
  `refreshOutputDeviceInfo()`, so toggling WASAPI `独占模式` or
  `稳定模式（高缓冲）` on the existing WASAPI player updates the displayed
  backend label instead of leaving the previous `WASAPI shared` text. Validation:
  Debug build passed with
  `scripts\build-app-msvc.cmd -BuildDir build-codex-asio-format-fallback
  -Configuration Debug -FfmpegAudioCoreRoot
  D:\AI\Codex\AudioPlayer-codex-0523-asio\build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc`.
  Build log:
  `build-claude-logs\build-app-msvc-91a653cddbe048c78709008fa9fddb63.log`.
  WASAPI shared smoke still passed with harness report
  `build-codex-asio-format-fallback\cache\logs\player-smoke-20260526-025717-236-52f88829.harness.json`;
  schema validation passed. This smoke does not directly inspect the dialog
  label, so the UI label assertion remains code-path validated rather than
  screenshot/manual verified.

## Status refresh: 2026-05-25

- User report: WASAPI exclusive/shared output could keep showing
  `正在重建播放管线（Xs）` even after playback had audibly resumed. Blame points
  the status-bar rebuild timer and WASAPI rebuild status messages to commit
  `5104830 Harden ASIO busy retry cleanup`.
- Root cause: the paused rearm path emitted `播放管线已重建`, but the active
  conservative rebuild path completed through `activeOutputSwitch
  output-active-summary` and `resetActiveOutputSwitch("output-active")`
  without emitting the matching completion status. `MainWindow` therefore kept
  its local rebuild timer running.
- Fix: when an active output-switch transaction reaches `output-active` after
  at least one conservative rebuild, WASAPI now logs
  `activeOutputSwitch rebuild-status-complete ...` and emits
  `播放管线已重建` before resetting the transaction. This is only a status/UI
  completion fix; it does not change WASAPI rebuild timing, buffering, or
  submitted PCM.
- Validation: build passed with
  `scripts/build-app.ps1 -BuildDir build-codex-0525-asio-retry2
  -Configuration Debug` after explicitly loading VS 2026 `vcvars64.bat`.
  Playable app:
  `build-codex-0525-asio-retry2\playable\Debug\20260525-050100\AudioPlayer.exe`.
- Validation: scripted same-output invalidation smoke passed assertions with
  `scripts/run-playback-smoke.ps1 -Source build-codex-0525-asio-retry2\fixtures\sine-1khz-minus18db-48k-stereo.wav
  -BuildDir build-codex-0525-asio-retry2 -Configuration Debug
  -RefreshOutputAfterMs 1000 -RefreshOutputTrigger SameOutputInvalidationIoError
  -RequirePlaying -RejectPlaybackErrors -RequireConservativeRebuildCount 1
  -RequireFirstDataBlockAfterConfigureCount 1
  -RequireLogPattern 'activeOutputSwitch rebuild-status-complete'
  -QuitAfterMs 7000`. Log
  `build-codex-0525-asio-retry2\cache\logs\player-smoke-20260525-050108-920-55c36208.log`
  showed one conservative rebuild, one ActiveSwitchRebuild pipeline, one first
  data block after configure, `output-active-summary`, then
  `activeOutputSwitch rebuild-status-complete`. Harness report
  `build-codex-0525-asio-retry2\cache\logs\player-smoke-20260525-050108-920-55c36208.harness.json`
  validated with `scripts/test-harness-reports.ps1 -Path ...55c36208.harness.json`.
  The harness result remained `INCONCLUSIVE` because endpoint output was not
  audibly/loopback verified.

## Status refresh: 2026-05-20

- Context-budget cleanup only; no WASAPI playback behavior change is intended.
- The WASAPI render worker and submitted-PCM diagnostic implementation moved
  from `src/backends/wasapi/windowswasapiaudioplayer.cpp` to
  `src/backends/wasapi/windowswasapiaudioplayer_worker.h`, while the high-level
  player control remains in `windowswasapiaudioplayer.cpp`.
- Older dated manual evidence and validation history through 2026-04-29 is
  archived in `docs/bug/archive/wasapi-anomaly-history-2026-05-20.md`.
- Validation: `scripts/test-harness-reports.ps1 -SelfTest` passed;
  `scripts/build-app.ps1 -BuildDir build-mm -Configuration Debug` passed with
  the existing `VCINSTALLDIR` deploy warning; the smoke command
  `scripts/run-playback-smoke.ps1` on `build-mm/fixtures/smoke.wav` with
  `-QuitAfterMs 3000 -RequirePlaying -RejectPlaybackErrors` passed with harness
  report
  `build-mm/cache/logs/player-smoke-20260520-031528-087-6daeb491.harness.json`;
  `scripts/test-harness-reports.ps1 -LatestSmoke` passed for that report.
- The smoke result is scripted playback/submitted-PCM evidence only. It does
  not prove endpoint output is pop/click free.

## Status refresh: 2026-05-19

- This file remains the source of truth for WASAPI anomaly scope and evidence
  limits. It is not a full changelog for later ASIO, playback-cache UI,
  release, or handoff-tool work.
- ASIO, playback-cache, and harness/report status now have separate trackers
  under `docs/bug/`; keep new non-WASAPI notes there unless a reproduced issue
  crosses into WASAPI behavior.
- Since the 2026-05-06 stability-mode status, the project has added or extended
  WASAPI exclusive-mode negotiation and refresh behavior, the Windows ASIO
  backend, playback cache controls, and harness evidence tooling such as report
  schema checks, evidence bundles, loopback alignment summaries,
  `ffprobe`-based duration checks, and playable-bundle smoke launches.
- The current project shape is multi-backend. Keep WASAPI shared, exclusive,
  stability-mode, and output-switch anomaly work scoped to WASAPI unless logs
  point to `AudioPlayerFactory`, UI state, playback-cache services, or another
  shared layer. Treat ASIO findings separately and do not carry WASAPI endpoint
  conclusions into ASIO without backend-specific evidence.
- No later local evidence in this file supersedes the 2026-05-06 read: no known
  remaining large app-side WASAPI fault is indicated. Continue treating large
  pop, playback reset, silence/no-sound, crash, failure to recover, submitted
  PCM discontinuity, stale session/buffer access, underrun, or decoder
  backpressure as app-side regressions. Light artifacts under Windows spatial
  audio, endpoint/APO invalidation, or system DPC/ISR pressure remain
  endpoint/system-layer risks unless future logs show a player-side fault.
- The current `docs/dev/harness.md` and related `docs/dev` pages are newer than
  much of the investigation history below. When harness mechanics or
  collaboration workflow details conflict with older dated notes, follow the
  durable `docs/dev` pages and the current sections near the top of this file.
- On 2026-05-20, older dated manual evidence and validation history was moved
  to `docs/bug/archive/wasapi-anomaly-history-2026-05-20.md` to reduce active
  context size.

## Current focus

- Use the harness/report contract as the validation baseline before guessing
  more playback fixes for audible spatial-audio or stale-tail artifacts.
- Smoke reports must use `PASS`, `FAIL`, or `INCONCLUSIVE` as the top-level
  result. Do not use `WARN` as a top-level result.
- Keep decoder/internal PCM, submitted backend PCM, and actual endpoint output as
  separate evidence layers.
- Treat submitted-PCM-clean results as useful diagnostics, not proof that actual
  speaker/headphone output is pop-free.
- Keep active output switching and error recovery separate while improving the
  diagnostics that describe them.

## Current priority order

1. Use the single-case smoke harness report contract as the baseline for new
   validation.
2. Use the regression harness aggregate report for case matrix summaries and
   evidence paths.
3. Keep output-switch, recovery, stale-buffer, underrun, submitted-PCM, and
   endpoint-output fields distinguishable in reports.
4. Return to active-switch playback changes only after the harness can describe
   failures without conflating evidence layers.

Do not default to small local patches if the problem is path coupling, unclear state, or unclear generation boundaries.

## Current harness status

- `scripts/run-playback-smoke.ps1` writes a script-owned `*.harness.json`
  report, preserving the app report result while normalizing the harness result
  to `PASS`, `FAIL`, or `INCONCLUSIVE`.
- App `WARN` maps to harness `INCONCLUSIVE`; warning context stays in
  `warnings`.
- `scripts/run-playback-regression.ps1` writes a regression aggregate report
  with per-case result counts and evidence paths.
- `scripts/run-loopback-manual-smoke.ps1` writes a wrapper-level summary JSON
  that aggregates the smoke harness report, app report, loopback aggregate and
  segment reports, loopback WAV paths, and manual observation. This summary is
  evidence aggregation only: it keeps `endpointOutputVerified=false` and
  `interpretation.canClaimNoPop=false`.
- Submitted-PCM-clean results still do not prove actual endpoint output is
  pop-free without manual listening or loopback capture.
- Current diagnostic focus is report visibility for active output switches,
  same-output invalidation, WASAPI recovery, stale buffer reuse, underrun, and
  submitted PCM peak/jump evidence without changing playback behavior.
- Next manual repro step: run `scripts/run-playback-smoke.ps1` with
  `-ManualObservationWindowSeconds` (or `-HoldSeconds`) and
  `-ManualObservationNote` while manually switching Windows Spatial Audio or
  output format. Use the resulting harness/app reports to align the note
  (`pop heard`, `no pop heard`, or `unclear`) with active-switch, recovery,
  stale-buffer, underrun, submitted-PCM, and endpoint-verification fields.

## Manual repro notes

Detailed dated manual and loopback notes through 2026-04-29 are archived in
`docs/bug/archive/wasapi-anomaly-history-2026-05-20.md`. Keep this active
section focused on current repro instructions and newest evidence only.

## Current preferred implementation scope

Prefer starting harness changes in:

- `scripts/run-playback-smoke.ps1`
- `scripts/run-playback-regression.ps1`
- `scripts/analyze-audio-artifacts.ps1`
- `docs/dev/harness.md`

Only change app CLI/report code when scripts cannot obtain the needed structured
evidence from existing logs or JSONL diagnostics.

Prefer starting playback-specific WASAPI anomaly changes in:

- `windowswasapiaudioplayer.h`
- `windowswasapiaudioplayer.cpp`
- `windowswasapiaudioplayer_worker.h`
- `windowswasapiaudioplayer_state.cpp`
- `windowswasapiaudioplayer_output.cpp`

For WASAPI exclusive-mode, stability-mode, or output-switch behavior, keep the
first patch inside the WASAPI backend unless the evidence points to shared UI,
factory, cache, or source-preparation state. ASIO regressions should start in
the ASIO backend and its diagnostics, not in this WASAPI anomaly path.

For the 2026-04-21 minimal continuity fix, the code change stayed localized to `windowswasapiaudioplayer.cpp`.

Do not expand first into these areas unless analysis shows it is necessary:

- `AudioPlayerFactory`
- other platform backends
- `FFmpegDecoderWorker`
- UI layer interfaces

## Pre-change analysis checklist

For tasks involving spatial-audio switch noise, output device switch anomalies, output reconfiguration discontinuity, stale-tail artifacts, or hot reconfigure anomalies, analyze before patching:

1. Current related state-transition flow.
2. Current implicit-state dependencies and important flags.
3. Which paths are active output switching.
4. Which paths are error recovery.
5. Which buffer, session, and generation lifetimes cross paths.
6. Current hot reconfigure eligibility conditions.
7. Which old flags, markers, or state fragments the change will delete, merge, or replace.

If a path cannot be mapped to the durable WASAPI principles in
`docs/dev/wasapi-switching.md`, prefer restructuring that path before adding
another local patch.

## Automation and log alignment

For iterative WASAPI output-change debugging, prefer targeted automation runs before the full regression sweep. Keep these scripts aligned with output reconfiguration paths:

- `scripts/run-playback-smoke.ps1`
- `scripts/run-playback-regression.ps1`
- `scripts/show-latest-anomalies.ps1`
- `scripts/analyze-audio-artifacts.ps1`

When changing switch or recovery semantics, logs should continue to distinguish:

- active output-switch transactions;
- error-recovery transactions;
- hot reconfigure hits;
- conservative rebuild fallback hits.

Use `scripts/analyze-audio-artifacts.ps1` for log-driven artifact attribution before changing playback logic based on a reproduced anomaly.

Historical validation notes and dated follow-ups through 2026-04-29 are archived in
`docs/bug/archive/wasapi-anomaly-history-2026-05-20.md`.

## Current acceptance bar

For this anomaly work, success is not only "the pop did not happen once." The working bar is:

- switch paths are readable;
- state transitions are enumerable;
- active switching and error recovery are separated;
- buffer, session, and generation lifecycle boundaries are clear;
- behavior is more explainable even when the chosen strategy is conservative.

## Communication rhythm

When the user has not asked for a detailed expansion:

- keep progress updates short;
- do not repeat already-confirmed background analysis;
- continue from the last confirmed direction;
- handle one clear subgoal per turn;
- expand detail only when changing scope, changing direction, or introducing new risk.

## Bit-depth precision output: 2026-05-30

- **Change**: Added source-bit-depth-first candidate ordering for WASAPI exclusive
  mode. `exclusivePcmCandidates()` now inserts source-matching encodings (e.g.
  Int16/16 for 16-bit source, Int24/24 for 24-bit source) before the existing
  fallback chain. First supported candidate still wins.
- **Change**: Shared mode `candidateSampleFormats()` now prefers source-matching
  Qt sample formats (Int16 for 16-bit, Int32 for 24/32-bit).
- **Change**: Added 2nd-order LNS (Lipshitz noise shaping) for 32→24 bit
  conversion in `copyConvertedFramesToRenderBuffer()`. The noise shaper state is
  per-channel and persists across render callbacks within a session. Reset on
  format change or session switch.
- **Interface**: `setSource()` now accepts `int sourceBitDepth` parameter. The
  value comes from `AudioInfo::bitDepthValue` (already probed by
  `playbacksourceservice_probe.cpp`).
- **Logging**: Exclusive candidate and final format logs now include
  `sourceBitDepth` and `bitDepthMatch=exact|fallback`.
- **Limitation**: Shared mode has no Int24 support (Qt limitation). 24-bit source
  in shared mode uses Int32 (32-bit container). Noise shaping only applies to
  32→24 conversion; 32→16 uses swresample's built-in TPDF dither.
- **Files changed**: `audioplayerbackend.h`, `windowswasapiaudioplayer.h`,
  `windowswasapiaudioplayer.cpp`, `windowswasapiaudioplayer_output.cpp`,
  `windowswasapiaudioplayer_worker.h`, `mainwindow_media.cpp`,
  `mainwindow_output.cpp`, `nativeaudioplayerstubbase.h`,
  `nativeaudioplayerstubbase.cpp`, `ffmpegaudioplayer.h`,
  `ffmpegaudioplayer.cpp`.

## 32→16 noise shaping (Phase 2a): 2026-05-31

- **Change**: `decoderFormatForOutput()` now upgrades Int16 to Int32, matching
  the existing Int24→Int32 pattern. Decoder outputs s32 for 16-bit devices.
- **Change**: `canRenderBufferFormatToDeviceFormat()` and active switch bridge
  guard now allow s32→s16 conversion.
- **Change**: Added `noiseShapedQuantize32To16()` (lsb=65536) and s32→s16
  conversion path in `copyConvertedFramesToRenderBuffer()`. Uses 2nd-order LNS
  with TPDF dither. Fallback uses rounding bias (±32768).
- **Automatic**: ffmpeg CLI args and libav in-process both produce s32 output
  automatically via `m_decoderPcmFormat` / `sampleFormatForOutput()`.
- **Files changed**: `windowswasapiaudioplayer_state.cpp`,
  `windowswasapiaudioplayer_output.cpp`, `windowswasapiaudioplayer_worker.h`.
