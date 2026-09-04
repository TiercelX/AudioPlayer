# Windows ASIO status

This file tracks Windows ASIO backend-specific behavior, validation, and
regressions. Keep durable workflow rules in `AGENTS.md` and `docs/dev/*.md`.

## FFmpeg audio-core version

- Current source version: **8.1.1** (updated 2026-05-29)
- Source location: `build-mm/ffmpeg-src`
- Full tool version: winget `Gyan.FFmpeg` 8.1.1 (for tool calls like `ffmpeg -f lavfi`)
- Before building, always check https://ffmpeg.org/releases/ for newer versions
- Do NOT assume the existing source is up-to-date

## Durable guidance

- Bug/status tracking index: `docs/bug/README.md`
- Workflow and change scope: `docs/dev/agent-workflow.md`
- Harness and smoke-test policy: `docs/dev/harness.md`
- Diagnostics and evidence layers: `docs/dev/diagnostics.md`
- Git and release workflow: `docs/dev/release-workflow.md`

## Status refresh: 2026-06-30 (shared libav Creative reorder alignment)

- **Shared decoder change**: `LibavSeekDecoderWorker` now applies Creative
  6/8-channel reorder after `swr_convert()` on final target-layout PCM instead
  of using a setup-time SWR rematrix. This aligns the ASIO libav path with the
  ASIO FFmpeg CLI `pan=` filter ordering.
- **Validation**: Unit tests `scripts\run-tests.ps1` passed, and app build
  `scripts\build-app.ps1 -BuildDir build-mm -Configuration Debug` passed.
- **Evidence limits**: No ASIO endpoint smoke/listening run was performed in
  this pass; the motivating evidence came from WASAPI G6 manual-test logs.

## Status refresh: 2026-06-04 (ASIO Slice E: AsioOutputWorker extraction)

- **Structure split**: Extracted AsioOutputWorker class (1578 lines) from
  windowsasioaudioplayer.cpp (3025 lines) to new files:
  - windowsasioaudioplayer_worker.h — class definition with shared globals
  - windowsasioaudioplayer_worker.cpp — global variable definitions and MOC
- **Main file reduced**: windowsasioaudioplayer.cpp now ~1600 lines (down from 3025).
- **Key changes**:
  - ASIO callback implementations moved from anonymous namespace to file-scope
  - NoiseShaperState and related functions namespaced under AsioWorker::
  - Shared globals (g_callbackWorker, g_callbackWorkerMutex) declared extern in header
- **Validation**:
  - Build: scripts\build-app-msvc.cmd -BuildDir build-mm -Configuration Debug — PASS
  - ASIO smoke: un-playback-smoke.ps1 -Source smoke.wav -AsioOutputIndex 0 — PASS
    - loadedBackend: Windows ASIO
    - sioSelectedDescription: Creative Sound Blaster ASIO Device
    - sioBackendStartVerified: True
    - submittedPcmConclusion: ASIO submitted PCM clean
- **Evidence limits**: Endpoint acoustic verification not performed (INCONCLUSIVE).
- **Branch**: opencode-0528 (current working branch).
## Status refresh: 2026-06-01 (noise-shaper correction + Realtek retest limit)

- **Change**: corrected the shared ASIO noise-shaper dither helper. Its unsigned
  subtraction wrapped the intended `-1` dither case to approximately `2^32`.
  The helper now emits signed two-bit TPDF dither.
- **Creative ASIO validation**: 16-bit fixture playback on Creative ASIO index 0
  reached `firstBufferSwitch`, entered `Active`, and reported
  `ASIO submitted PCM clean`. Command:
  `scripts\run-playback-smoke.ps1 -Source
  'build-codex-wasapi-bitdepth\fixtures\sine-1khz-minus18db-48k-stereo.wav'
  -BuildDir build-codex-wasapi-bitdepth -Configuration Debug -AsioOutputIndex 0
  -QuitAfterMs 11000 -RequirePlaying -RejectPlaybackErrors
  -RequireAudibleLevels -NoCleanup`. Report:
  `build-codex-wasapi-bitdepth/cache/logs/player-smoke-20260601-113034-024-c4b420c8.harness.json`.
  Harness result remains `INCONCLUSIVE` at the endpoint-acoustic evidence layer;
  the application report is `PASS`.
- **Realtek ASIO retest limitation**: the same fixture on Realtek ASIO index 1
  failed before render. STA `safeAsioInit` returned `initResult=0` for all host
  window variants across five retries; MTA `CoCreateInstance` then failed.
  Report:
  `build-codex-wasapi-bitdepth/cache/logs/player-smoke-20260601-113052-521-85a5c46c.harness.json`.
  Log:
  `build-codex-wasapi-bitdepth/cache/logs/player-smoke-20260601-113052-521-85a5c46c.log`.
  This is independent of the WASAPI clipping repair and should be investigated
  as a separate Realtek driver-initialization issue.


## Status refresh: 2026-06-04 (ASIO Slice E: AsioOutputWorker extraction)

- **Structure split**: Extracted AsioOutputWorker class (1578 lines) from
  windowsasioaudioplayer.cpp (3025 lines) to new files:
  - windowsasioaudioplayer_worker.h — class definition with shared globals
  - windowsasioaudioplayer_worker.cpp — global variable definitions and MOC
- **Main file reduced**: windowsasioaudioplayer.cpp now ~1600 lines (down from 3025).
- **Key changes**:
  - ASIO callback implementations moved from anonymous namespace to file-scope
  - NoiseShaperState and related functions namespaced under AsioWorker::
  - Shared globals (g_callbackWorker, g_callbackWorkerMutex) declared extern in header
- **Validation**:
  - Build: scripts\build-app-msvc.cmd -BuildDir build-mm -Configuration Debug — PASS
  - ASIO smoke: un-playback-smoke.ps1 -Source smoke.wav -AsioOutputIndex 0 — PASS
    - loadedBackend: Windows ASIO
    - sioSelectedDescription: Creative Sound Blaster ASIO Device
    - sioBackendStartVerified: True
    - submittedPcmConclusion: ASIO submitted PCM clean
- **Evidence limits**: Endpoint acoustic verification not performed (INCONCLUSIVE).
- **Branch**: opencode-0528 (current working branch).
## Status refresh: 2026-05-30 (ASIO multi-device warning + exclusive cooldown)

- **Change**: Added multi-device detection for ASIO drivers. When user selects an
  ASIO device and multiple physical devices from the same vendor are connected
  (e.g. Sound BlasterX G5 + G6 both connected), a warning dialog is shown.
  Confirming the dialog just rebuilds the menu without switching backend,
  preserving current playback state.
- **Change**: Added 300ms cooldown between WASAPI exclusive and ASIO backend
  switches (both directions). This lets the device driver settle after releasing
  exclusive mode or ASIO driver, matching the existing ASIO-to-ASIO cooldown
  (`kAsioReleaseCooldownMs`).
- **Root cause**: Creative ASIO driver uses a single CLSID for all devices. When
  multiple Creative devices are connected, the ASIO driver's `bufferSwitch`
  callback never fires, causing a 15-second recovery timeout loop. The driver
  initializes successfully but cannot route audio to a specific physical device.
- **Detection**: `detectMultiplePhysicalDevicesForAsioDriver()` in
  `src/backends/asio/windowsasioaudioplayer_sessionprobe.cpp` enumerates WASAPI
  render endpoints matching vendor keywords (Creative/Sound Blaster/BlasterX/G5/G6,
  Realtek) and returns all matches. Generic approach works for any ASIO driver
  with known vendor keywords.
- **Files changed**:
  - `src/backends/asio/windowsasioaudioplayer_sessionprobe.h` (new struct + declaration)
  - `src/backends/asio/windowsasioaudioplayer_sessionprobe.cpp` (detection implementation)
  - `src/ui/mainwindow_output.cpp` (dialog + cooldown logic)
- **Build**: `scripts\build-app-msvc.cmd -BuildDir build-device-health-check
  -Configuration Debug -FfmpegAudioCoreRoot build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc`.
  Build log: `build-claude-logs\build-app-msvc-0a7e7b56adea4eb59d01dbec5ea1d5d4.log`.
- **Validation**: Manual test with G5+G6 connected. Dialog appears on ASIO Creative
  selection. Confirm preserves current playback. Menu checkmark restores correctly.
- **Branch**: `opencode-0528` (current working branch).


## Status refresh: 2026-06-04 (ASIO Slice E: AsioOutputWorker extraction)

- **Structure split**: Extracted AsioOutputWorker class (1578 lines) from
  windowsasioaudioplayer.cpp (3025 lines) to new files:
  - windowsasioaudioplayer_worker.h — class definition with shared globals
  - windowsasioaudioplayer_worker.cpp — global variable definitions and MOC
- **Main file reduced**: windowsasioaudioplayer.cpp now ~1600 lines (down from 3025).
- **Key changes**:
  - ASIO callback implementations moved from anonymous namespace to file-scope
  - NoiseShaperState and related functions namespaced under AsioWorker::
  - Shared globals (g_callbackWorker, g_callbackWorkerMutex) declared extern in header
- **Validation**:
  - Build: scripts\build-app-msvc.cmd -BuildDir build-mm -Configuration Debug — PASS
  - ASIO smoke: un-playback-smoke.ps1 -Source smoke.wav -AsioOutputIndex 0 — PASS
    - loadedBackend: Windows ASIO
    - sioSelectedDescription: Creative Sound Blaster ASIO Device
    - sioBackendStartVerified: True
    - submittedPcmConclusion: ASIO submitted PCM clean
- **Evidence limits**: Endpoint acoustic verification not performed (INCONCLUSIVE).
- **Branch**: opencode-0528 (current working branch).
## Status refresh: 2026-05-30 (exit crash + hot-recovery validation)

- **Exit crash validation**: Creative ASIO exit crash (0xC0000005) is no longer
  reproducible. All ASIO driver calls (`safeAsioInit`, `safeAsioStop`,
  `safeAsioDisposeBuffers`, `safeAsioRelease`) are now protected with SEH
  (`__try/__except`) in `windowsasioaudioplayer_discovery.cpp` and
  `windowsasioaudioplayer_utils.cpp`. Process exits cleanly with code 0 after
  ASIO playback.
- **Validation**: `scripts\run-playback-smoke.ps1 -BuildDir build-opencode-asio-utils
  -Configuration Debug -Source build-opencode-asio-utils\fixtures\sine-1khz-minus18db-48k-stereo.wav
  -AsioOutputIndex 0 -QuitAfterMs 11000 -RequirePlaying -RejectPlaybackErrors`.
  Report: `build-opencode-asio-utils\cache\logs\player-smoke-20260530-101105-933-e387248f.harness.json`.
  Observed: `exit=0`, `reportResult=PASS`, `asioFirstBufferSwitchObserved=True`.
- **Hot-recovery validation**: ASIO hot-switch recovery now works. When another
  WASAPI session occupies the same endpoint (Sound BlasterX G5), ASIO detects
  the occupation via `checkWasapiSessionsForEndpoint()` and retries
  (`session-retry`). After the occupier stops, ASIO successfully initializes and
  starts playback without app restart.
- **Test script fix**: Updated `scripts\test-asio-hot-switch.ps1` to:
  1. Accept `-OccupierOutputDeviceIndex` parameter (default=2 for Sound BlasterX G5)
  2. Detect `session-retry` log pattern for occupation detection
  3. Stop ASIO early once retry is detected (instead of waiting full timeout)
- **Validation**: `scripts\test-asio-hot-switch.ps1 -BuildDir build-opencode-asio-utils
  -Configuration Debug -Source build-opencode-asio-utils\fixtures\sine-1khz-minus18db-48k-stereo.wav
  -AsioOutputIndex 0 -OccupierOutputDeviceIndex 2`.
  Result: `Occupied-blocked=True`, `Recovery-init-ok=True`, `Recovery-buffer=True`, `PASS`.
- **Status update**: Moving "Creative ASIO exit crash" and "Creative ASIO
  hot-recovery" from unresolved issues to resolved.
- **Branch**: `opencode-0528` (current working branch).


## Status refresh: 2026-06-04 (ASIO Slice E: AsioOutputWorker extraction)

- **Structure split**: Extracted AsioOutputWorker class (1578 lines) from
  windowsasioaudioplayer.cpp (3025 lines) to new files:
  - windowsasioaudioplayer_worker.h — class definition with shared globals
  - windowsasioaudioplayer_worker.cpp — global variable definitions and MOC
- **Main file reduced**: windowsasioaudioplayer.cpp now ~1600 lines (down from 3025).
- **Key changes**:
  - ASIO callback implementations moved from anonymous namespace to file-scope
  - NoiseShaperState and related functions namespaced under AsioWorker::
  - Shared globals (g_callbackWorker, g_callbackWorkerMutex) declared extern in header
- **Validation**:
  - Build: scripts\build-app-msvc.cmd -BuildDir build-mm -Configuration Debug — PASS
  - ASIO smoke: un-playback-smoke.ps1 -Source smoke.wav -AsioOutputIndex 0 — PASS
    - loadedBackend: Windows ASIO
    - sioSelectedDescription: Creative Sound Blaster ASIO Device
    - sioBackendStartVerified: True
    - submittedPcmConclusion: ASIO submitted PCM clean
- **Evidence limits**: Endpoint acoustic verification not performed (INCONCLUSIVE).
- **Branch**: opencode-0528 (current working branch).
## Status refresh: 2026-05-30 (AsioOutputWorker mapping + sampleTypeBytes extraction)

- **Change**: Completed read-only `AsioOutputWorker` dependency mapping and
  extracted `sampleTypeBytes` + `AsioSampleType` enum to `AsioUtils` namespace.
- **Worker mapping result**: Only 1 method (`sampleTypeBytes`) was directly
  extractable. 4 more (`normalizedSample`, `writeChannel`, `clearAsioBuffers`,
  `classifyEndpointOpenFailure`) could become pure with signature changes but
  are low priority. All other methods are tightly coupled to worker state.
- **Files changed**:
  - `src/backends/asio/windowsasioaudioplayer_utils.h` (added `AsioSampleType` enum + `sampleTypeBytes`)
  - `src/backends/asio/windowsasioaudioplayer_utils.cpp` (added `sampleTypeBytes` impl)
  - `src/backends/asio/windowsasioaudioplayer.cpp` (removed enum + method, updated call sites)
  - `docs/dev/asio-code-map.md` (AsioOutputWorker mapping section)
- **Build**: `scripts\build-app-msvc.cmd -BuildDir build-opencode-asio-utils
  -Configuration Debug -FfmpegAudioCoreRoot
  E:\AI\OpenCode\AudioPlayer\build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc`.
  Build log:
  `build-claude-logs\build-app-msvc-01c1c3f32d9d41fe9f0cfa8a7d4d34b8.log`.
- **Validation**: Creative ASIO index 0 smoke passed.
  Report: `build-opencode-asio-utils\cache\logs\player-smoke-20260530-095509-094-1fae4d69.harness.json`.
  Observed: `reportResult=PASS`, `submittedPcmConclusion=ASIO submitted PCM clean`.
- **Branch**: `opencode-0528` (current working branch).


## Status refresh: 2026-06-04 (ASIO Slice E: AsioOutputWorker extraction)

- **Structure split**: Extracted AsioOutputWorker class (1578 lines) from
  windowsasioaudioplayer.cpp (3025 lines) to new files:
  - windowsasioaudioplayer_worker.h — class definition with shared globals
  - windowsasioaudioplayer_worker.cpp — global variable definitions and MOC
- **Main file reduced**: windowsasioaudioplayer.cpp now ~1600 lines (down from 3025).
- **Key changes**:
  - ASIO callback implementations moved from anonymous namespace to file-scope
  - NoiseShaperState and related functions namespaced under AsioWorker::
  - Shared globals (g_callbackWorker, g_callbackWorkerMutex) declared extern in header
- **Validation**:
  - Build: scripts\build-app-msvc.cmd -BuildDir build-mm -Configuration Debug — PASS
  - ASIO smoke: un-playback-smoke.ps1 -Source smoke.wav -AsioOutputIndex 0 — PASS
    - loadedBackend: Windows ASIO
    - sioSelectedDescription: Creative Sound Blaster ASIO Device
    - sioBackendStartVerified: True
    - submittedPcmConclusion: ASIO submitted PCM clean
- **Evidence limits**: Endpoint acoustic verification not performed (INCONCLUSIVE).
- **Branch**: opencode-0528 (current working branch).
## Status refresh: 2026-05-30 (Day 5 utility/state-name helpers extraction)

- **Change**: Extracted 19 pure helper functions from
  `src/backends/asio/windowsasioaudioplayer.cpp` into
  `src/backends/asio/windowsasioaudioplayer_utils.cpp` / `.h` under the
  `AsioUtils` namespace.
- **Moved functions**: `playbackStateName`, `audioStateName`,
  `toolExecutableOverride`, `hwndText`, `asioDriverError`, `asioResultOk`,
  `boundedEnvInt`, `safeAsioStart`, `safeAsioStop`, `safeAsioCanSampleRate`,
  `safeAsioSetSampleRate`, `safeAsioGetSampleRate`, `safeAsioGetChannels`,
  `safeAsioGetLatencies`, `safeAsioGetBufferSize`, `safeAsioGetChannelInfo`,
  `safeAsioCreateBuffers`, `safeAsioDisposeBuffers`, `safeAsioOutputReady`.
- **Moved constants**: `kAsioOk`, `kAsioSuccess`, `kAsioNotPresent` to
  `AsioUtils` namespace.
- **Files changed**:
  - `src/backends/asio/windowsasioaudioplayer_utils.cpp` (new)
  - `src/backends/asio/windowsasioaudioplayer_utils.h` (new)
  - `src/backends/asio/windowsasioaudioplayer.cpp` (removed functions, updated call sites)
  - `CMakeLists.txt` (added new source files)
- **Build**: `scripts\build-app-msvc.cmd -BuildDir build-opencode-asio-utils
  -Configuration Debug -FfmpegAudioCoreRoot
  E:\AI\OpenCode\AudioPlayer\build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc`.
  Build log:
  `build-claude-logs\build-app-msvc-4eb580d064c545f8be283d47b684d46a.log`.
- **Validation**: Creative ASIO index 0 smoke passed with
  `scripts\run-playback-smoke.ps1 -BuildDir build-opencode-asio-utils
  -Configuration Debug -Source build-opencode-asio-utils\fixtures\sine-1khz-minus18db-48k-stereo.wav
  -AsioOutputIndex 0 -QuitAfterMs 18000 -RequirePlaying`.
  Report: `build-opencode-asio-utils\cache\logs\player-smoke-20260530-092937-688-2c9a115f.harness.json`.
  Observed: `loadedBackend=Windows ASIO`, `asioSelectedDescription=Creative Sound Blaster ASIO Device`,
  `asioConfigureRate=48000`, `asioActiveSampleRate=48000`, `asioFirstBufferSwitchObserved=True`,
  `reportResult=PASS`, `submittedPcmConclusion=ASIO submitted PCM clean`.
- **Validation**: Realtek ASIO index 1 smoke passed with same build.
  Report: `build-opencode-asio-utils\cache\logs\player-smoke-20260530-093008-543-19ec8cfc.harness.json`.
  Observed: `asioSelectedDescription=Realtek ASIO`, `asioConfigureRate=48000`,
  `asioActiveSampleRate=48000`, `asioFirstBufferSwitchObserved=True`, `reportResult=PASS`.
- **Branch**: `opencode-0528` (current working branch).


## Status refresh: 2026-06-04 (ASIO Slice E: AsioOutputWorker extraction)

- **Structure split**: Extracted AsioOutputWorker class (1578 lines) from
  windowsasioaudioplayer.cpp (3025 lines) to new files:
  - windowsasioaudioplayer_worker.h — class definition with shared globals
  - windowsasioaudioplayer_worker.cpp — global variable definitions and MOC
- **Main file reduced**: windowsasioaudioplayer.cpp now ~1600 lines (down from 3025).
- **Key changes**:
  - ASIO callback implementations moved from anonymous namespace to file-scope
  - NoiseShaperState and related functions namespaced under AsioWorker::
  - Shared globals (g_callbackWorker, g_callbackWorkerMutex) declared extern in header
- **Validation**:
  - Build: scripts\build-app-msvc.cmd -BuildDir build-mm -Configuration Debug — PASS
  - ASIO smoke: un-playback-smoke.ps1 -Source smoke.wav -AsioOutputIndex 0 — PASS
    - loadedBackend: Windows ASIO
    - sioSelectedDescription: Creative Sound Blaster ASIO Device
    - sioBackendStartVerified: True
    - submittedPcmConclusion: ASIO submitted PCM clean
- **Evidence limits**: Endpoint acoustic verification not performed (INCONCLUSIVE).
- **Branch**: opencode-0528 (current working branch).
## Status refresh: 2026-05-30 (expand sample rate candidate list)

- **Change**: Expanded `commonRatesDescending` candidate list in both ASIO and
  WASAPI exclusive backends. Added 768000, 705600, 320000, 282240, 256000,
  128000, 64000, 32000, 22050, 16000, 11025 to the probe list. Both backends
  now cover the full range of standard 44.1k/48k family rates from 11025 to
  768000 Hz.
- **Rationale**: The previous list had a gap between 192k and 384k (only 352.8k)
  and no coverage below 44.1k or above 384k. The ASIO SDK has no rate
  enumeration API — `canSampleRate()` only probes one rate at a time — so the
  candidate list must be comprehensive.
- **Files changed**:
  - `src/backends/asio/windowsasioaudioplayer_formats.cpp` (line 111)
  - `src/backends/wasapi/windowswasapiaudioplayer_output.cpp` (line 442)
- **Build**: `scripts\build-app-msvc.cmd -BuildDir build-samplerate-expand
  -Configuration Debug -FfmpegAudioCoreRoot
  build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc`.
- **Validation**: 4 smoke tests passed:
  - Realtek ASIO (index 1) 48kHz source: `asioConfigureRate=48000`,
    `asioActiveSampleRate=48000`, `reportResult=PASS`.
  - Creative ASIO (index 0) 48kHz source: `asioConfigureRate=48000`,
    `asioActiveSampleRate=48000`, `reportResult=PASS`.
  - Realtek ASIO (index 1) 44.1kHz source: `asioConfigureRate=44100`,
    `asioActiveSampleRate=48000` (fallback — Realtek driver rejects 44.1kHz),
    `reportResult=PASS`. Log confirms probe chain: 44100→32000→22050→16000→11025
    all rejected (`result=-995`), 48000 accepted.
  - Creative ASIO (index 0) 44.1kHz source: `asioConfigureRate=44100`,
    `asioActiveSampleRate=44100` (direct match), `reportResult=PASS`.
- **Branch**: `opencode-realtek-asio` (current working branch).


## Status refresh: 2026-06-04 (ASIO Slice E: AsioOutputWorker extraction)

- **Structure split**: Extracted AsioOutputWorker class (1578 lines) from
  windowsasioaudioplayer.cpp (3025 lines) to new files:
  - windowsasioaudioplayer_worker.h — class definition with shared globals
  - windowsasioaudioplayer_worker.cpp — global variable definitions and MOC
- **Main file reduced**: windowsasioaudioplayer.cpp now ~1600 lines (down from 3025).
- **Key changes**:
  - ASIO callback implementations moved from anonymous namespace to file-scope
  - NoiseShaperState and related functions namespaced under AsioWorker::
  - Shared globals (g_callbackWorker, g_callbackWorkerMutex) declared extern in header
- **Validation**:
  - Build: scripts\build-app-msvc.cmd -BuildDir build-mm -Configuration Debug — PASS
  - ASIO smoke: un-playback-smoke.ps1 -Source smoke.wav -AsioOutputIndex 0 — PASS
    - loadedBackend: Windows ASIO
    - sioSelectedDescription: Creative Sound Blaster ASIO Device
    - sioBackendStartVerified: True
    - submittedPcmConclusion: ASIO submitted PCM clean
- **Evidence limits**: Endpoint acoustic verification not performed (INCONCLUSIVE).
- **Branch**: opencode-0528 (current working branch).
## Status refresh: 2026-05-30 (Realtek ASIO fix — COM apartment pre-init)

- **Root cause found**: Realtek ASIO `init()` was failing because Qt or another
  library pre-initializes COM as MTA on the audio worker thread. When
  `openDriver()` tries STA, `CoInitializeEx` returns `RPC_E_CHANGED_MODE`
  (0x80010106), causing STA to be skipped. The fallback MTA attempt fails at
  `CoCreateInstance` with `E_NOINTERFACE` (0x80004002) because the Realtek
  driver's COM class is registered with `Apartment` threading model and requires
  STA.
- **Previous incorrect conclusion**: The 2026-05-29 investigation concluded the
  Realtek driver was "broken at the driver level" because `init()` returned
  `kAsioFalse` for all host window candidates. The actual cause was the COM
  apartment mismatch, not a driver defect.
- **Probe tool validation**: Built `tools/realtek-asio-probe/` with ASIO SDK
  headers. The probe confirmed:
  - Realtek ASIO `init()` passes with ALL 5 window handle types (app-window,
    desktop, message-window, null-handle) when COM is STA.
  - Realtek ASIO `init()` passes on both main thread and worker thread with STA.
  - MTA pre-init simulation reproduces the exact failure: `RPC_E_CHANGED_MODE`
    on STA → `E_NOINTERFACE` on MTA `CoCreateInstance`.
  - The ASIO SDK's loading mechanism (`CoCreateInstance` with same CLSID for
    both class and interface) is identical to the project's approach.
- **Fix**: Modified `openDriver()` in `src/backends/asio/windowsasioaudioplayer.cpp`
  to detect `RPC_E_CHANGED_MODE` when attempting STA. Instead of skipping STA,
  the code now calls `CoUninitialize()` to clear the pre-existing MTA state,
  then retries `CoInitializeEx(COINIT_APARTMENTTHREADED)`. This is safe because
  the audio worker thread is dedicated to ASIO and owns its COM lifecycle.
- **Validation**: Debug build passed with
  `scripts\build-app-msvc.cmd -BuildDir build-realtek-fix -Configuration Debug
  -FfmpegAudioCoreRoot build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc`.
- **Validation**: Realtek ASIO index 1 smoke passed with
  `scripts\run-playback-smoke.ps1 -BuildDir build-realtek-fix -Configuration
  Debug -Source build-realtek-fix\fixtures\sine-1khz-minus18db-48k-stereo.wav
  -AsioOutputIndex 1 -QuitAfterMs 18000 -RequirePlaying`.
  Report: `build-realtek-fix\cache\logs\player-smoke-20260530-084719-123-0460c928.harness.json`.
  Observed: `loadedBackend=Windows ASIO`, `asioSelectedDescription=Realtek ASIO`,
  `asioConfigureRate=48000`, `asioActiveSampleRate=48000`,
  `asioFirstBufferSwitchObserved=True`, `reportResult=PASS`,
  `submittedPcmConclusion=ASIO submitted PCM clean`.
- **Validation**: Creative ASIO index 0 smoke also passed with same build.
  Report: `build-realtek-fix\cache\logs\player-smoke-20260530-084748-306-8a7af7cb.harness.json`.
  Observed: `asioSelectedDescription=Creative Sound Blaster ASIO Device`,
  `asioFirstBufferSwitchObserved=True`, `reportResult=PASS`.
- **Files changed**:
  - `src/backends/asio/windowsasioaudioplayer.cpp` (COM apartment fix in `openDriver()`)
  - `tools/realtek-asio-probe/` (diagnostic probe tool, new)
- **Branch**: `opencode-realtek-asio` (current working branch).


## Status refresh: 2026-06-04 (ASIO Slice E: AsioOutputWorker extraction)

- **Structure split**: Extracted AsioOutputWorker class (1578 lines) from
  windowsasioaudioplayer.cpp (3025 lines) to new files:
  - windowsasioaudioplayer_worker.h — class definition with shared globals
  - windowsasioaudioplayer_worker.cpp — global variable definitions and MOC
- **Main file reduced**: windowsasioaudioplayer.cpp now ~1600 lines (down from 3025).
- **Key changes**:
  - ASIO callback implementations moved from anonymous namespace to file-scope
  - NoiseShaperState and related functions namespaced under AsioWorker::
  - Shared globals (g_callbackWorker, g_callbackWorkerMutex) declared extern in header
- **Validation**:
  - Build: scripts\build-app-msvc.cmd -BuildDir build-mm -Configuration Debug — PASS
  - ASIO smoke: un-playback-smoke.ps1 -Source smoke.wav -AsioOutputIndex 0 — PASS
    - loadedBackend: Windows ASIO
    - sioSelectedDescription: Creative Sound Blaster ASIO Device
    - sioBackendStartVerified: True
    - submittedPcmConclusion: ASIO submitted PCM clean
- **Evidence limits**: Endpoint acoustic verification not performed (INCONCLUSIVE).
- **Branch**: opencode-0528 (current working branch).
## Status refresh: 2026-05-29 (Realtek ASIO investigation)

- **Branch**: `opencode-realtek-asio` created for Realtek ASIO investigation.
- **Realtek ASIO driver detected**: Registry shows `Realtek ASIO` with CLSID
  `{A80362FF-CE76-4DD9-874A-704C57BF0D6A}` and DLL `rthdasio64.dll` (version 3.1.15.3).
- **Session probe working**: `resolveWasapiEndpointForAsioDriver` correctly maps
  Realtek ASIO to WASAPI endpoint.
- **COM threading model**: Realtek driver registered with `Apartment` threading model.
- **Initialization failure**: Realtek ASIO `init()` fails with all host window
  candidates (app-window, desktop, message-window, null-handle) in STA mode.
  MTA mode fails at `CoCreateInstance` with `E_NOINTERFACE` (0x80004002).
- **Error messages**: `asioDriverError()` returns empty string for Realtek driver.
- **Root cause identified**: Realtek ASIO driver (`rthdasio64.dll`) does not support
  standard ASIO `init()` calls. The driver returns `kAsioFalse` (0) for all host
  window types without providing error messages. This is a driver-level limitation.
- **Code improvements**:
  1. Modified `openDriver()` to continue trying other COM models when
     `CoCreateInstance` fails.
  2. Added `message-window` (HWND_MESSAGE) as a new host window candidate.
  3. Fixed session probe to ignore `pid=0` system sessions that were incorrectly
     counted as external sessions.
- **Creative ASIO works**: Same code path successfully initializes Creative Sound
  Blaster ASIO (index 0) with `reportResult=PASS`.
- **Validation**: Debug build passed with
  `scripts\build-app-msvc.cmd -BuildDir build-realtek-asio -Configuration Debug
  -FfmpegAudioCoreRoot build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc`.
- **Validation**: Creative ASIO index 0 smoke passed.
- **Validation**: Realtek ASIO index 1 smoke failed - driver `init()` returns 0.
- **Conclusion**: Realtek ASIO driver (v3.1.15.3) is broken at the driver level.
  The driver does not implement standard ASIO `init()` correctly. This is a known
  limitation of Realtek ASIO drivers, which are "notoriously limited" as noted in
  ASIO community discussions.
- **Recommendation**: Do not use Realtek ASIO. Use WASAPI shared/exclusive mode
  instead for Realtek audio devices. ASIO is only recommended for professional
  audio interfaces like Creative Sound Blaster or dedicated ASIO devices.


## Status refresh: 2026-06-04 (ASIO Slice E: AsioOutputWorker extraction)

- **Structure split**: Extracted AsioOutputWorker class (1578 lines) from
  windowsasioaudioplayer.cpp (3025 lines) to new files:
  - windowsasioaudioplayer_worker.h — class definition with shared globals
  - windowsasioaudioplayer_worker.cpp — global variable definitions and MOC
- **Main file reduced**: windowsasioaudioplayer.cpp now ~1600 lines (down from 3025).
- **Key changes**:
  - ASIO callback implementations moved from anonymous namespace to file-scope
  - NoiseShaperState and related functions namespaced under AsioWorker::
  - Shared globals (g_callbackWorker, g_callbackWorkerMutex) declared extern in header
- **Validation**:
  - Build: scripts\build-app-msvc.cmd -BuildDir build-mm -Configuration Debug — PASS
  - ASIO smoke: un-playback-smoke.ps1 -Source smoke.wav -AsioOutputIndex 0 — PASS
    - loadedBackend: Windows ASIO
    - sioSelectedDescription: Creative Sound Blaster ASIO Device
    - sioBackendStartVerified: True
    - submittedPcmConclusion: ASIO submitted PCM clean
- **Evidence limits**: Endpoint acoustic verification not performed (INCONCLUSIVE).
- **Branch**: opencode-0528 (current working branch).
## Status refresh: 2026-05-29 (ASIO error reporting + progress bar fix)

- **ASIO error reporting**: Added `m_lastOpenFailureDetail` to `AsioOutputWorker`
  that stores the specific failure step when `openDriver()`, `startOutput()`,
  or recovery paths fail. Error messages now include the specific failure reason
  (e.g., "driver->init() failed", "No supported sample rate", "createBuffers failed").
  This helps diagnose Realtek ASIO and other driver-specific failures.
- **Progress bar fix**: Fixed progress bar jumping to 0 during backend switch
  (e.g., ASIO → WASAPI exclusive). Root cause: `m_player->stop()` emitted
  `positionChanged(0)` before `m_pendingSeekPosition` was set, causing the
  slider to briefly show 0. Fix: set `m_pendingSeekPosition` before calling
  `stop()` in `switchOutputBackendAndDevice()`.
- **Known limitation**: Realtek ASIO drivers are notoriously limited. The improved
  error messages will show the specific failure step, but the driver may still fail
  at `init()` or other steps. The session probe (`resolveWasapiEndpointForAsioDriver`)
  only maps Creative/Sound Blaster endpoints, so device-busy detection does not work
  for Realtek ASIO.
- Validation: Debug build passed with `scripts\build-app.ps1 -Configuration Debug`.
- Files changed:
  - `src/backends/asio/windowsasioaudioplayer.cpp`
  - `src/ui/mainwindow_output.cpp`
  - `src/ui/mainwindow.cpp`


## Status refresh: 2026-06-04 (ASIO Slice E: AsioOutputWorker extraction)

- **Structure split**: Extracted AsioOutputWorker class (1578 lines) from
  windowsasioaudioplayer.cpp (3025 lines) to new files:
  - windowsasioaudioplayer_worker.h — class definition with shared globals
  - windowsasioaudioplayer_worker.cpp — global variable definitions and MOC
- **Main file reduced**: windowsasioaudioplayer.cpp now ~1600 lines (down from 3025).
- **Key changes**:
  - ASIO callback implementations moved from anonymous namespace to file-scope
  - NoiseShaperState and related functions namespaced under AsioWorker::
  - Shared globals (g_callbackWorker, g_callbackWorkerMutex) declared extern in header
- **Validation**:
  - Build: scripts\build-app-msvc.cmd -BuildDir build-mm -Configuration Debug — PASS
  - ASIO smoke: un-playback-smoke.ps1 -Source smoke.wav -AsioOutputIndex 0 — PASS
    - loadedBackend: Windows ASIO
    - sioSelectedDescription: Creative Sound Blaster ASIO Device
    - sioBackendStartVerified: True
    - submittedPcmConclusion: ASIO submitted PCM clean
- **Evidence limits**: Endpoint acoustic verification not performed (INCONCLUSIVE).
- **Branch**: opencode-0528 (current working branch).
## Status refresh: 2026-05-29 (opencode Day 4 completion)

- Day 4 Track C completed: post-ASIO consolidation
- Updated `docs/dev/asio-code-map.md` with Day 5 ready checklist
- Identified next safe ASIO split: utility/state-name helpers extraction
- Created task card for `windowsasioaudioplayer_utils.cpp` with exact move candidates
- Alternative: read-only AsioOutputWorker mapping for future worker extraction
- Validation: `git diff --check` passed for documentation changes
- Branch: `opencode-0528` (current working branch)
- No build required — documentation-only task
- No endpoint audio behavior changed


## Status refresh: 2026-06-04 (ASIO Slice E: AsioOutputWorker extraction)

- **Structure split**: Extracted AsioOutputWorker class (1578 lines) from
  windowsasioaudioplayer.cpp (3025 lines) to new files:
  - windowsasioaudioplayer_worker.h — class definition with shared globals
  - windowsasioaudioplayer_worker.cpp — global variable definitions and MOC
- **Main file reduced**: windowsasioaudioplayer.cpp now ~1600 lines (down from 3025).
- **Key changes**:
  - ASIO callback implementations moved from anonymous namespace to file-scope
  - NoiseShaperState and related functions namespaced under AsioWorker::
  - Shared globals (g_callbackWorker, g_callbackWorkerMutex) declared extern in header
- **Validation**:
  - Build: scripts\build-app-msvc.cmd -BuildDir build-mm -Configuration Debug — PASS
  - ASIO smoke: un-playback-smoke.ps1 -Source smoke.wav -AsioOutputIndex 0 — PASS
    - loadedBackend: Windows ASIO
    - sioSelectedDescription: Creative Sound Blaster ASIO Device
    - sioBackendStartVerified: True
    - submittedPcmConclusion: ASIO submitted PCM clean
- **Evidence limits**: Endpoint acoustic verification not performed (INCONCLUSIVE).
- **Branch**: opencode-0528 (current working branch).
## Status refresh: 2026-05-29 (opencode format/session split)

- Structure split: extracted format helpers and session probing from
  `src/backends/asio/windowsasioaudioplayer.cpp` into
  `src/backends/asio/windowsasioaudioplayer_formats.cpp` and
  `src/backends/asio/windowsasioaudioplayer_sessionprobe.cpp`.
- Moved to `AsioFormats` namespace: `pcmCodecName`, `pcmSampleFormatName`,
  `pcmMuxerName`, `pcmStreamFormatFromQAudioFormat`, `appendUniqueSampleRate`,
  `sourcePreferredSampleRateCandidates`, `sampleMagnitude`.
- Moved to `AsioSessionProbe` namespace: `isCreativeAsioDriver`,
  `resolveWasapiEndpointForAsioDriver`, `isAudioEndpointBusy`,
  `checkWasapiSessionsForEndpoint`, `hasExternalWasapiRenderSessionsForAsioDriver`,
  `hasActiveExternalWasapiRenderSessionsForAsioDriver`,
  `hasAnyExternalWasapiRenderSessionsForAsioDriver`, `WasapiSessionCheckResult`.
- Updated callers in `windowsasioaudioplayer.cpp` to use new namespaces.
- Validation: Debug build passed with
  `scripts\build-app-msvc.cmd -BuildDir build-opencode-asio-format
  -Configuration Debug -FfmpegAudioCoreRoot
  E:\AI\OpenCode\AudioPlayer\build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc`.
  Build log:
  `build-claude-logs\build-app-msvc-08eec46a76bb4bb5afcdd7bffa7015c5.log`.
- Validation: Creative ASIO index 0 smoke passed with
  `scripts\run-playback-smoke.ps1 -BuildDir build-opencode-asio-format
  -Configuration Debug -Source build-opencode-asio-format\fixtures\sine-1khz-5s.wav
  -AsioOutputIndex 0 -QuitAfterMs 18000 -RequirePlaying`.
  Report: `build-opencode-asio-format\cache\logs\player-smoke-20260529-111645-944-3ec85281.harness.json`.
  Observed: `loadedBackend=Windows ASIO`, `asioSelectedDescription=Creative Sound Blaster ASIO Device`,
  `asioConfigureRate=44100`, `asioActiveSampleRate=44100`, `asioFirstBufferSwitchObserved=True`,
  `reportResult=PASS`, `harnessResult=INCONCLUSIVE` (endpoint audio verification not collected).
- Branch: `opencode-MiMo-0530-asio-format-session`.


## Status refresh: 2026-06-04 (ASIO Slice E: AsioOutputWorker extraction)

- **Structure split**: Extracted AsioOutputWorker class (1578 lines) from
  windowsasioaudioplayer.cpp (3025 lines) to new files:
  - windowsasioaudioplayer_worker.h — class definition with shared globals
  - windowsasioaudioplayer_worker.cpp — global variable definitions and MOC
- **Main file reduced**: windowsasioaudioplayer.cpp now ~1600 lines (down from 3025).
- **Key changes**:
  - ASIO callback implementations moved from anonymous namespace to file-scope
  - NoiseShaperState and related functions namespaced under AsioWorker::
  - Shared globals (g_callbackWorker, g_callbackWorkerMutex) declared extern in header
- **Validation**:
  - Build: scripts\build-app-msvc.cmd -BuildDir build-mm -Configuration Debug — PASS
  - ASIO smoke: un-playback-smoke.ps1 -Source smoke.wav -AsioOutputIndex 0 — PASS
    - loadedBackend: Windows ASIO
    - sioSelectedDescription: Creative Sound Blaster ASIO Device
    - sioBackendStartVerified: True
    - submittedPcmConclusion: ASIO submitted PCM clean
- **Evidence limits**: Endpoint acoustic verification not performed (INCONCLUSIVE).
- **Branch**: opencode-0528 (current working branch).
## Status refresh: 2026-05-29 (opencode discovery split)

- Structure split: extracted pure discovery helpers from
  `src/backends/asio/windowsasioaudioplayer.cpp` into
  `src/backends/asio/windowsasioaudioplayer_discovery.cpp` and
  `src/backends/asio/windowsasioaudioplayer_discovery.h`.
- Extracted IASIO interface into `src/backends/asio/asio_interface.h`.
- Moved functions: `utf16StringFromRegistryValue`, `appendAsioRegistryEntries`,
  `registeredAsioDrivers`, `enumAsioHostWindow`, `asioHostWindow`,
  `asioHostWindowCandidates`, `parseClsid`, `createAsioDriver`, `safeAsioInit`,
  `safeAsioRelease`.
- Validation: Debug build passed with
  `scripts\build-app-msvc.cmd -BuildDir build-opencode-asio-discovery
  -Configuration Debug -FfmpegAudioCoreRoot
  E:\AI\OpenCode\AudioPlayer\build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc`.
  Build log:
  `build-claude-logs\build-app-msvc-c411f3ee4819438399a5e6865626d383.log`.
- Validation: Creative ASIO index 0 smoke passed with
  `scripts\run-playback-smoke.ps1 -BuildDir build-opencode-asio-discovery
  -Configuration Debug -Source build-opencode-asio-discovery\fixtures\sine-1khz-10s.wav
  -AsioOutputIndex 0 -QuitAfterMs 18000 -RequirePlaying`.
  Report: `build-opencode-asio-discovery\cache\logs\player-smoke-20260529-103840-289-b25b3d48.harness.json`.
  Observed: `loadedBackend=Windows ASIO`, `asioSelectedDescription=Creative Sound Blaster ASIO Device`,
  `asioConfigureRate=44100`, `asioActiveSampleRate=44100`, `asioFirstBufferSwitchObserved=True`,
  `reportResult=PASS`, `harnessResult=INCONCLUSIVE` (endpoint audio verification not collected).
- Branch: `opencode-MiMo-0529-asio-discovery`.
- Commit: `ea997a3`.


## Status refresh: 2026-06-04 (ASIO Slice E: AsioOutputWorker extraction)

- **Structure split**: Extracted AsioOutputWorker class (1578 lines) from
  windowsasioaudioplayer.cpp (3025 lines) to new files:
  - windowsasioaudioplayer_worker.h — class definition with shared globals
  - windowsasioaudioplayer_worker.cpp — global variable definitions and MOC
- **Main file reduced**: windowsasioaudioplayer.cpp now ~1600 lines (down from 3025).
- **Key changes**:
  - ASIO callback implementations moved from anonymous namespace to file-scope
  - NoiseShaperState and related functions namespaced under AsioWorker::
  - Shared globals (g_callbackWorker, g_callbackWorkerMutex) declared extern in header
- **Validation**:
  - Build: scripts\build-app-msvc.cmd -BuildDir build-mm -Configuration Debug — PASS
  - ASIO smoke: un-playback-smoke.ps1 -Source smoke.wav -AsioOutputIndex 0 — PASS
    - loadedBackend: Windows ASIO
    - sioSelectedDescription: Creative Sound Blaster ASIO Device
    - sioBackendStartVerified: True
    - submittedPcmConclusion: ASIO submitted PCM clean
- **Evidence limits**: Endpoint acoustic verification not performed (INCONCLUSIVE).
- **Branch**: opencode-0528 (current working branch).
## Status refresh: 2026-05-28

- Structure split follow-up: a Claude-driven attempt to split ASIO helper code
  on branch `Claude-0528-asio-helpers` was rejected and not landed. The attempt
  crossed the safe helper-only boundary by corrupting `AsioOutputWorker`
  placement, generating duplicate class definitions, and creating temporary
  unapproved `scripts\fix-corrupt.*` files.
- Validation: the rejected ASIO split failed to build with
  `scripts\build-app-msvc.cmd -BuildDir build-structure-asio -Configuration
  Debug -FfmpegAudioCoreRoot
  D:\AI\Codex\AudioPlayer-codex-0523-asio\build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc`.
  Build log:
  `D:\AI\Codex\AudioPlayer-Claude-0528-asio-helpers\build-claude-logs\build-app-msvc-3e79b0f93d9f488dbb87b1ae91a19a48.log`.
  No ASIO structure code from that branch was committed or merged. Next ASIO
  structure work should be narrower than Slice D, such as one helper family per
  task with an immediate build before extracting another family.
- Branch-integration follow-up: did not cherry-pick the `codex-0416` commits as a
  block, because that branch predates the ASIO sample-rate fallback/libav path.
  The current branch keeps ASIO sample-rate fallback and ASIO libav decoding,
  while adopting the newer libav file resolver from the 0416 line.
- CMake now requires the in-process libav decoder through
  `AUDIOPLAYER_REQUIRE_LIBAV_DECODER` on default app builds and resolves each
  required library as either `<name>.lib` or `lib<name>.a`. The existing
  self-built audio-core bundled-tool requirement remains in place for playable
  packages.
- Validation: Debug build passed with the complete self-built audio-core root at
  `D:\AI\Codex\AudioPlayer-codex-0523-asio\build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc`.
  Build log:
  `build-claude-logs\build-app-msvc-056e4eccee4a423ebc91df46f8807806.log`.
- Validation: no ASIO-device smoke was run in this pass. A WASAPI shared smoke
  passed after the build:
  `build-codex-asio-format-fallback\cache\logs\player-smoke-20260528-014046-634-2ada0e98.harness.json`.
  ASIO endpoint behavior and actual endpoint output remain covered only by the
  earlier ASIO-specific validation notes until rerun.


## Status refresh: 2026-06-04 (ASIO Slice E: AsioOutputWorker extraction)

- **Structure split**: Extracted AsioOutputWorker class (1578 lines) from
  windowsasioaudioplayer.cpp (3025 lines) to new files:
  - windowsasioaudioplayer_worker.h — class definition with shared globals
  - windowsasioaudioplayer_worker.cpp — global variable definitions and MOC
- **Main file reduced**: windowsasioaudioplayer.cpp now ~1600 lines (down from 3025).
- **Key changes**:
  - ASIO callback implementations moved from anonymous namespace to file-scope
  - NoiseShaperState and related functions namespaced under AsioWorker::
  - Shared globals (g_callbackWorker, g_callbackWorkerMutex) declared extern in header
- **Validation**:
  - Build: scripts\build-app-msvc.cmd -BuildDir build-mm -Configuration Debug — PASS
  - ASIO smoke: un-playback-smoke.ps1 -Source smoke.wav -AsioOutputIndex 0 — PASS
    - loadedBackend: Windows ASIO
    - sioSelectedDescription: Creative Sound Blaster ASIO Device
    - sioBackendStartVerified: True
    - submittedPcmConclusion: ASIO submitted PCM clean
- **Evidence limits**: Endpoint acoustic verification not performed (INCONCLUSIVE).
- **Branch**: opencode-0528 (current working branch).
## Status refresh: 2026-05-26 (Codex source-rate fallback)

- User report: after MiMo's source-audio-rate ASIO experiment, switching from
  ASIO to WASAPI and output-format display became suspect; a 352.8 kHz source
  could still show 352.8 kHz even though the Sound BlasterX G5 endpoint is known
  to top out at 192 kHz. Desired behavior is to request the source rate when
  possible, but automatically fall back to the best supported output format and
  show the actual selected output format.
- Follow-up user report: after selecting ASIO, clicking the playback-menu
  WASAPI `独占模式` did not switch back to WASAPI, and clicking `稳定模式（高缓冲）`
  could leave the checkbox ticked without actually entering WASAPI stable mode.
- Follow-up fix: restored the 0416-era UI mode-switch path. The menu actions now
  call a `switchToWasapiMode()` helper that changes the backend to WASAPI before
  applying exclusive or stability mode. `refreshOutputDeviceInfo()` no longer
  keeps the stability checkbox ticked while the active backend is ASIO.
- Fix: playback backends now receive the probed source sample rate. ASIO uses it
  as the first requested driver rate, probes common fallback rates in descending
  quality below the source rate, and updates the player/UI output format after
  the driver confirms the actual rate. WASAPI exclusive candidate ordering now
  also starts with the source rate and falls back through common rates instead
  of only using the mix-format-first list.
- Validation: build passed with
  `scripts\build-app-msvc.cmd -BuildDir build-codex-asio-format-fallback
  -Configuration Debug -DeployFfmpegExecutable D:\Tool\ffmpeg\bin\ffmpeg.exe
  -DeployFfprobeExecutable D:\Tool\ffmpeg\bin\ffprobe.exe`. Playable app:
  `build-codex-asio-format-fallback\playable\Debug\20260526-021400\AudioPlayer.exe`.
- Validation: generated a local 352.8 kHz fixture at
  `build-codex-asio-format-fallback\fixtures\sine-1khz-352800-stereo-s24.wav`.
  WASAPI shared smoke with `-ListOutputDevices -RequirePlaying
  -RejectPlaybackErrors` loaded the source with `sampleRate=352800` but selected
  the current shared mix format at 96 kHz. Harness report:
  `build-codex-asio-format-fallback\cache\logs\player-smoke-20260526-021453-333-b5f67a36.harness.json`.
- Validation: Creative ASIO index 0 smoke with
  `scripts\run-playback-smoke.ps1 -Source build-codex-asio-format-fallback\fixtures\sine-1khz-352800-stereo-s24.wav
  -BuildDir build-codex-asio-format-fallback -Configuration Debug
  -AsioOutputIndex 0 -QuitAfterMs 18000 -RequireLogPattern 'asio sampleRate'
  -FfmpegPathOverride D:\Tool\ffmpeg\bin\ffmpeg.exe
  -FfprobePathOverride D:\Tool\ffmpeg\bin\ffprobe.exe` observed
  `asio sampleRate candidate ... candidate=352800 result=-995`, then
  `candidate=192000 result=0`, `asio sampleRate fallback ... selected=192000`,
  and `asio outputFormat resolved requestedRate=352800 actualRate=192000`.
  Harness report
  `build-codex-asio-format-fallback\cache\logs\player-smoke-20260526-021510-344-6a8d2b09.harness.json`
  recorded `asioConfigureRate=352800`, `asioActiveSampleRate=192000`,
  `asioFirstBufferSwitchObserved=True`, clean ASIO submitted PCM, and
  `harnessResult=INCONCLUSIVE` only because endpoint-output verification was not
  collected. The report schema check passed with
  `scripts\test-harness-reports.ps1 -Path ...6a8d2b09.harness.json`.
- Follow-up validation after restoring the UI switch path: rebuild passed with
  playable app
  `build-codex-asio-format-fallback\playable\Debug\20260526-022131\AudioPlayer.exe`.
  Creative ASIO index 0 smoke still observed 352.8 kHz -> 192 kHz fallback with
  first buffer callback and clean submitted PCM:
  `build-codex-asio-format-fallback\cache\logs\player-smoke-20260526-022146-890-98e2588d.harness.json`
  (`harnessResult=INCONCLUSIVE`, endpoint output unverified). A parallel WASAPI
  smoke failed while ASIO held the G5 endpoint (`AUDCLNT_E_DEVICE_IN_USE`), so it
  was rerun separately; the separate WASAPI stable shared smoke passed:
  `build-codex-asio-format-fallback\cache\logs\player-smoke-20260526-022217-987-3b3776f0.harness.json`.
- Startup default follow-up: app startup no longer restores the previous ASIO
  backend/device or WASAPI stability checkbox from settings. Startup now resets
  to the system backend, which is WASAPI shared on Windows, using the default
  output device. Default startup smoke confirmed `loadedBackend=WASAPI shared`,
  `usesDefault=1`, and `exclusiveMode=0 stabilityMode=0` in
  `build-codex-asio-format-fallback\cache\logs\player-smoke-20260526-022818-128-806cf10e.harness.json`.
- Build-contract follow-up: default Windows app builds no longer allow the
  ASIO/WASAPI validation path to use full external `ffmpeg.exe`/`ffprobe.exe`
  deploy overrides or silently omit libav. `scripts\build-app.ps1` now requires
  a complete self-built `runtime-with-ffprobe-msvc` audio-core root containing
  bundled tools, headers, and MSVC libav import libraries; CMake enforces the
  same rule with `AUDIOPLAYER_REQUIRE_FFMPEG_AUDIO_CORE=ON`.
- Build-contract validation: the previous external-tool command using
  `D:\Tool\ffmpeg\bin\ffmpeg.exe` and `ffprobe.exe` was rejected immediately.
  Rebuild passed only when pointed at the complete self-built root
  `D:\AI\Codex\AudioPlayer-codex-0523-asio\build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc`.
  Build log:
  `build-claude-logs\build-app-msvc-7399746394354f81b5089f78cab2b95d.log`.
  Playable app:
  `build-codex-asio-format-fallback\playable\Debug\20260526-023830\AudioPlayer.exe`.
- Package validation: the playable package retained only the self-built
  `ffmpeg.exe`/`ffprobe.exe` and `windowsmediaplugind.dll`; Qt's FFmpeg
  multimedia plugin and `avcodec`/`avformat`/`avutil`/`swresample`/`swscale`
  DLL payloads were pruned. A no-override WASAPI smoke passed with harness
  report
  `build-codex-asio-format-fallback\cache\logs\player-smoke-20260526-023930-426-915d7408.harness.json`;
  schema validation passed.
- ASIO decoder follow-up: ASIO now shares the packaged-libav codec gate with
  WASAPI and can start `LibavSeekDecoderWorker` for supported packaged codecs
  instead of always spawning `ffmpeg.exe`. The media info decoder label is now
  dynamic (`libav (in-process)` or `ffmpeg CLI`) instead of the misleading fixed
  string `ffmpeg + ASIO`; `Windows ASIO` remains the output backend label.
- ASIO libav validation: rebuild passed with
  `scripts\build-app-msvc.cmd -BuildDir build-codex-asio-format-fallback
  -Configuration Debug -FfmpegAudioCoreRoot
  D:\AI\Codex\AudioPlayer-codex-0523-asio\build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc`.
  Build log:
  `build-claude-logs\build-app-msvc-11434a0c8181461a93163a6efa529d1d.log`.
  WASAPI shared smoke on the 352.8 kHz PCM fixture still used
  `decoder-start mode=libav-inprocess` and passed:
  `build-codex-asio-format-fallback\cache\logs\player-smoke-20260526-025240-175-770588cf.harness.json`.
- ASIO libav validation: Creative ASIO index 0 smoke on the same fixture
  required log pattern `asio startPipeline decoder-start mode=libav-inprocess`
  and observed 352.8 kHz -> 192 kHz fallback, prepared output, Active state,
  first buffer switch, `libavSeek startDecoding`, and clean ASIO submitted PCM.
  Harness report:
  `build-codex-asio-format-fallback\cache\logs\player-smoke-20260526-025251-700-94337e3c.harness.json`.
  Result remains `INCONCLUSIVE` only because endpoint-output verification was
  not collected; schema validation passed for both new smoke reports.


## Status refresh: 2026-06-04 (ASIO Slice E: AsioOutputWorker extraction)

- **Structure split**: Extracted AsioOutputWorker class (1578 lines) from
  windowsasioaudioplayer.cpp (3025 lines) to new files:
  - windowsasioaudioplayer_worker.h — class definition with shared globals
  - windowsasioaudioplayer_worker.cpp — global variable definitions and MOC
- **Main file reduced**: windowsasioaudioplayer.cpp now ~1600 lines (down from 3025).
- **Key changes**:
  - ASIO callback implementations moved from anonymous namespace to file-scope
  - NoiseShaperState and related functions namespaced under AsioWorker::
  - Shared globals (g_callbackWorker, g_callbackWorkerMutex) declared extern in header
- **Validation**:
  - Build: scripts\build-app-msvc.cmd -BuildDir build-mm -Configuration Debug — PASS
  - ASIO smoke: un-playback-smoke.ps1 -Source smoke.wav -AsioOutputIndex 0 — PASS
    - loadedBackend: Windows ASIO
    - sioSelectedDescription: Creative Sound Blaster ASIO Device
    - sioBackendStartVerified: True
    - submittedPcmConclusion: ASIO submitted PCM clean
- **Evidence limits**: Endpoint acoustic verification not performed (INCONCLUSIVE).
- **Branch**: opencode-0528 (current working branch).
## Status refresh: 2026-05-25 (Codex layered merge validation)

- Merge branch `codex-0525-layered-into-claude-0523` was created from
  `claude-0523`, then merged `codex-0523-asio`,
  `Claude-0525-asio-followup`, and `codex-0525-asio-retry-feedback` in order.
  The first merge intentionally resolved duplicate Claude workflow/build-script
  conflicts in favor of `codex-0523-asio`; the final tree matches
  `codex-0525-asio-retry-feedback`.
- Validation: `scripts/bootstrap-dev-env.ps1 -CheckOnly` found Git, CMake, Qt,
  MSVC, MSYS2, and host FFmpeg. The new worktree did not contain its own
  `build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc` runtime, so the app
  build used the existing self-built `ffmpeg.exe`/`ffprobe.exe` from
  `D:\AI\Codex\AudioPlayer-codex-0523-asio\build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc\bin`.
- Validation: Release build passed with
  `cmd /c scripts\build-app-msvc.cmd -BuildDir build-mm -Configuration Release
  -DeployFfmpegExecutable ...\ffmpeg.exe -DeployFfprobeExecutable ...\ffprobe.exe`.
  Build log:
  `build-claude-logs\build-app-msvc-5fa402f82ee04a97b5d58957895cd445.log`.
  Playable app:
  `build-mm\playable\Release\20260525-052141\AudioPlayer.exe`.
- Validation: generated fixtures with
  `scripts\ensure-playback-fixtures.ps1 -BuildDir build-mm`, then ran WASAPI
  smoke with `scripts\run-playback-smoke.ps1 -BuildDir build-mm -Configuration
  Release -Source build-mm\fixtures\smoke.wav -QuitAfterMs 3000
  -RequirePlaying -RejectPlaybackErrors -FfmpegPathOverride ...\ffmpeg.exe
  -FfprobePathOverride ...\ffprobe.exe`. Harness report
  `build-mm\cache\logs\player-smoke-20260525-052206-865-b5043f14.harness.json`
  recorded `harnessResult=PASS`; endpoint pop/click verification remained
  `INCONCLUSIVE`, as no manual listening or loopback endpoint evidence was
  collected.


## Status refresh: 2026-06-04 (ASIO Slice E: AsioOutputWorker extraction)

- **Structure split**: Extracted AsioOutputWorker class (1578 lines) from
  windowsasioaudioplayer.cpp (3025 lines) to new files:
  - windowsasioaudioplayer_worker.h — class definition with shared globals
  - windowsasioaudioplayer_worker.cpp — global variable definitions and MOC
- **Main file reduced**: windowsasioaudioplayer.cpp now ~1600 lines (down from 3025).
- **Key changes**:
  - ASIO callback implementations moved from anonymous namespace to file-scope
  - NoiseShaperState and related functions namespaced under AsioWorker::
  - Shared globals (g_callbackWorker, g_callbackWorkerMutex) declared extern in header
- **Validation**:
  - Build: scripts\build-app-msvc.cmd -BuildDir build-mm -Configuration Debug — PASS
  - ASIO smoke: un-playback-smoke.ps1 -Source smoke.wav -AsioOutputIndex 0 — PASS
    - loadedBackend: Windows ASIO
    - sioSelectedDescription: Creative Sound Blaster ASIO Device
    - sioBackendStartVerified: True
    - submittedPcmConclusion: ASIO submitted PCM clean
- **Evidence limits**: Endpoint acoustic verification not performed (INCONCLUSIVE).
- **Branch**: opencode-0528 (current working branch).
## Status refresh: 2026-05-25 (Codex ASIO recovery callback follow-up)

- User report: after pausing Bilibili/Edge, the status bar could show
  `ASIO 播放已恢复` even though playback did not actually resume, while
  `正在恢复 ASIO 播放（0s/15s）` kept flashing. The timed retry/recovery text
  could also advance non-linearly because backend status messages and the UI's
  local status timer were both trying to drive the displayed elapsed time.
- Fix: ASIO callback-watchdog and driver-reset recovery now remain in a pending
  recovery state after `safeAsioStart()`/`ActiveState`. They only emit
  `ASIO 播放已恢复` after the first ASIO `bufferSwitch` callback is observed.
  A restart that reaches `ActiveState` but still receives no callback is logged
  as `restart-pending-callback` and continues the same 15-second recovery
  window.
- Fix: recovery timeout is now reported as `RecoveryTimeout` with the user
  message `ASIO 播放恢复超时（15 秒），请暂停或关闭其他音频应用后重试`, instead of
  being folded into the generic device-busy message.
- Fix: `MainWindow` now treats repeated ASIO timed status messages as state
  keepalives, not as display clock updates. A new timed state starts a local
  monotonic `QElapsedTimer`; repeated backend messages with the same prefix and
  timeout no longer reset or redraw the status bar, so the visible counter
  should progress linearly. A follow-up changed the status timer to a 250 ms
  `Qt::PreciseTimer` while only redrawing on integer-second changes, reducing
  visible jitter without letting repeated backend messages reset the clock.
- Validation: build passed with
  `scripts/build-app.ps1 -BuildDir build-codex-0525-asio-retry2
  -Configuration Debug` after explicitly loading VS 2026 `vcvars64.bat` and
  adding MSVC, Windows Kit, and Qt CMake paths. Playable app:
  `build-codex-0525-asio-retry2\playable\Debug\20260525-044551\AudioPlayer.exe`.
- Validation: Creative ASIO index 0 smoke with
  `scripts/run-playback-smoke.ps1 -Source build-codex-0525-asio-retry2\fixtures\sine-1khz-minus18db-48k-stereo.wav
  -BuildDir build-codex-0525-asio-retry2 -Configuration Debug
  -ListOutputDevices -AsioOutputIndex 0
  -RequireErrorPattern 'ASIO 播放恢复超时' -QuitAfterMs 22000` passed its
  expected-error assertions. Log
  `build-codex-0525-asio-retry2\cache\logs\player-smoke-20260525-044600-863-8971915a.log`
  showed configure/prepare/`ActiveState`, no `firstBufferSwitch`, nine
  `callbackWatchdogRecovery restart-pending-callback` attempts, and
  `callbackWatchdogRecovery timeout session=1 count=9 elapsed=16470ms`.
  The log did not contain `ASIO 播放已恢复`; the final automation error was
  `ASIO 播放恢复超时（15 秒），请暂停或关闭其他音频应用后重试`.
- Validation: harness report
  `build-codex-0525-asio-retry2\cache\logs\player-smoke-20260525-044600-863-8971915a.harness.json`
  recorded `harnessResult=INCONCLUSIVE`, `asioAudioStateActiveObserved=True`,
  and `asioFirstBufferSwitchObserved=False`, which is expected for this
  no-callback recovery-timeout path and does not prove endpoint output recovery.
  `scripts/test-harness-reports.ps1 -Path ...8971915a.harness.json` returned
  `harness:PASS`.
- Follow-up validation after the user reported closing Edge still did not allow
  ASIO playback: the same current build later passed ASIO index 0
  `-RequirePlaying -RejectPlaybackErrors` smoke. Command:
  `scripts/run-playback-smoke.ps1 -Source build-codex-0525-asio-retry2\fixtures\sine-1khz-minus18db-48k-stereo.wav
  -BuildDir build-codex-0525-asio-retry2 -Configuration Debug
  -ListOutputDevices -AsioOutputIndex 0 -RequirePlaying -RejectPlaybackErrors
  -QuitAfterMs 24000`. Log
  `build-codex-0525-asio-retry2\cache\logs\player-smoke-20260525-045806-858-982c184e.log`
  showed `wasapiSessionCheck clear`, `asio configureOutput`,
  `asio prepareOutput`, `asio firstBufferSwitch session=1`, and
  `asio audioStateChanged ... state=Active error=0`. Harness report
  `build-codex-0525-asio-retry2\cache\logs\player-smoke-20260525-045806-858-982c184e.harness.json`
  validated with `scripts/test-harness-reports.ps1 -Path ...982c184e.harness.json`.
  This rules out a deterministic app playback-path break in that environment,
  but it does not explain the user's manual Edge-after-timeout failure or prove
  endpoint audible output.


## Status refresh: 2026-06-04 (ASIO Slice E: AsioOutputWorker extraction)

- **Structure split**: Extracted AsioOutputWorker class (1578 lines) from
  windowsasioaudioplayer.cpp (3025 lines) to new files:
  - windowsasioaudioplayer_worker.h — class definition with shared globals
  - windowsasioaudioplayer_worker.cpp — global variable definitions and MOC
- **Main file reduced**: windowsasioaudioplayer.cpp now ~1600 lines (down from 3025).
- **Key changes**:
  - ASIO callback implementations moved from anonymous namespace to file-scope
  - NoiseShaperState and related functions namespaced under AsioWorker::
  - Shared globals (g_callbackWorker, g_callbackWorkerMutex) declared extern in header
- **Validation**:
  - Build: scripts\build-app-msvc.cmd -BuildDir build-mm -Configuration Debug — PASS
  - ASIO smoke: un-playback-smoke.ps1 -Source smoke.wav -AsioOutputIndex 0 — PASS
    - loadedBackend: Windows ASIO
    - sioSelectedDescription: Creative Sound Blaster ASIO Device
    - sioBackendStartVerified: True
    - submittedPcmConclusion: ASIO submitted PCM clean
- **Evidence limits**: Endpoint acoustic verification not performed (INCONCLUSIVE).
- **Branch**: opencode-0528 (current working branch).
## Status refresh: 2026-05-25 (Codex ASIO busy feedback follow-up)

- User report: Apple Music pause appears to keep the Creative/Sound Blaster
  endpoint unavailable to ASIO, while Edge/Bilibili pause may release after a
  short wait. Treat paused-but-still-present external WASAPI sessions as a
  possible ASIO occupancy signal when ASIO open/start fails; do not assume pause
  means the endpoint is available.
- Fix: `checkWasapiSessionsForEndpoint()` now records all external sessions and
  logs each session state. The startup preflight still waits on active external
  sessions before opening ASIO, but ASIO open/create/start failures now
  re-check for any external session on the mapped endpoint and classify that
  failure as `DeviceBusy` instead of a generic driver recovery failure. This
  keeps Apple Music paused/retained-session cases on the 15-second busy retry
  path.
- Fix: retry status now uses a backend helper so repeated `play()` while retry
  is active emits the same timed `ASIO 设备被占用，正在重试（Xs/15s）` message
  instead of an untimed ellipsis-only message.
- Fix: ASIO callback-watchdog and reset recovery emit timed
  `正在恢复 ASIO 播放（Xs/15s）` status messages and `ASIO 播放已恢复` on success.
  `MainWindow` treats ASIO busy retry and ASIO recovery as timed status-bar
  states, carries over the elapsed seconds from backend messages, and keeps
  ASIO busy timeout errors visible for 18 seconds.
- Validation: build passed with
  `scripts/build-app.ps1 -BuildDir build-codex-0525-asio-retry2
  -Configuration Debug` after explicitly loading VS 2026 `vcvars64.bat` and
  adding MSVC, Windows Kit, and Qt CMake paths. The build reused the existing
  audio-core runtime from
  `D:\AI\Codex\AudioPlayer-codex-0523-asio\build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc\bin`.
  Playable app:
  `build-codex-0525-asio-retry2\playable\Debug\20260525-033732\AudioPlayer.exe`.
- Validation: fixtures generated with
  `scripts/ensure-playback-fixtures.ps1 -BuildDir build-codex-0525-asio-retry2`.
  Non-ASIO list/play smoke passed with harness report
  `build-codex-0525-asio-retry2\cache\logs\player-smoke-20260525-033805-312-6a301abd.harness.json`.
- Validation: Creative ASIO index 0 with the current external endpoint session
  present failed to start under `-RequirePlaying`, as expected for an occupied
  endpoint. Log
  `build-codex-0525-asio-retry2\cache\logs\player-smoke-20260525-033814-706-146b6c9d.log`
  showed mapped endpoint
  `{0.0.0.00000000}.{67bad92e-2d15-4e47-9aba-e09f5299c8d1}`,
  `activeCount=1`, `externalCount=2`, session states
  `pid=1792 state=Active,pid=0 state=Inactive`, and repeated
  `asio startPipeline session-retry`.
- Validation: occupied-endpoint expected-error smoke passed assertions with
  `scripts/run-playback-smoke.ps1 -Source build-codex-0525-asio-retry2\fixtures\sine-1khz-minus18db-48k-stereo.wav
  -BuildDir build-codex-0525-asio-retry2 -Configuration Debug
  -ListOutputDevices -AsioOutputIndex 0
  -RequireErrorPattern 'ASIO 设备仍被其他应用占用' -QuitAfterMs 22000`.
  Log
  `build-codex-0525-asio-retry2\cache\logs\player-smoke-20260525-033838-320-cb11c391.log`
  showed 31 retry attempts over about 15.45 seconds, then
  `session-check-timeout` and error
  `ASIO 设备仍被其他应用占用（已重试 15 秒），请暂停或关闭其他音频应用后重试`.
  Harness report
  `build-codex-0525-asio-retry2\cache\logs\player-smoke-20260525-033838-320-cb11c391.harness.json`
  recorded `expectedErrorObserved=true`, no failure reasons, and
  `result=INCONCLUSIVE` because ASIO backend start/submitted-output/endpoint
  output were not verified in an occupied-device run.
  `scripts/test-harness-reports.ps1 -Path ...cb11c391.harness.json` passed.


## Status refresh: 2026-06-04 (ASIO Slice E: AsioOutputWorker extraction)

- **Structure split**: Extracted AsioOutputWorker class (1578 lines) from
  windowsasioaudioplayer.cpp (3025 lines) to new files:
  - windowsasioaudioplayer_worker.h — class definition with shared globals
  - windowsasioaudioplayer_worker.cpp — global variable definitions and MOC
- **Main file reduced**: windowsasioaudioplayer.cpp now ~1600 lines (down from 3025).
- **Key changes**:
  - ASIO callback implementations moved from anonymous namespace to file-scope
  - NoiseShaperState and related functions namespaced under AsioWorker::
  - Shared globals (g_callbackWorker, g_callbackWorkerMutex) declared extern in header
- **Validation**:
  - Build: scripts\build-app-msvc.cmd -BuildDir build-mm -Configuration Debug — PASS
  - ASIO smoke: un-playback-smoke.ps1 -Source smoke.wav -AsioOutputIndex 0 — PASS
    - loadedBackend: Windows ASIO
    - sioSelectedDescription: Creative Sound Blaster ASIO Device
    - sioBackendStartVerified: True
    - submittedPcmConclusion: ASIO submitted PCM clean
- **Evidence limits**: Endpoint acoustic verification not performed (INCONCLUSIVE).
- **Branch**: opencode-0528 (current working branch).
## Status refresh: 2026-05-24 (Codex MiMo cleanup)

- Codex reviewed the dirty MiMo ASIO recovery state and removed the unreliable
  isolated-host leftovers. `WindowsAsioProcessAudioPlayer` source files were
  removed from the worktree, `CMakeLists.txt` no longer references them, the
  factory keeps using direct `WindowsAsioAudioPlayer`, and the hidden
  `--asio-host-play` CLI path was removed. The standalone `--asio-probe-driver`
  diagnostic entry remains.
- ASIO busy retry is now generation-guarded. A stop, source change, output
  device change, or new playback start invalidates older retry timers, so a
  stale 500 ms retry cannot silently restart playback for an old source/device.
  The self-capturing `shared_ptr<std::function>` retry loop was replaced with
  generation-checked `QTimer::singleShot` attempts.
- Busy retry no longer sets backend state to `Playing` while it is only waiting
  for another app to release the endpoint. The UI still receives status-bar
  retry messages, and ASIO start evidence remains the first `bufferSwitch`
  callback.
- Validation: `scripts\build-app-msvc.cmd -BuildDir build-codex-asio-review
  -Configuration Debug` failed before compile because that new build directory
  had no self-built audio-core ffmpeg/ffprobe runtime. The successful validation
  reused the existing audio-core runtime in `build-mimo-asio`:
  `scripts\build-app-msvc.cmd -BuildDir build-mimo-asio -Configuration Debug`
  passed and produced
  `build-mimo-asio\playable\Debug\20260524-234857\AudioPlayer.exe`. Build log:
  `build-claude-logs\build-app-msvc-0f2e8def8d674a608af70b50bb0d14e9.log`.
- Validation: `powershell -NoProfile -ExecutionPolicy Bypass -File
  scripts\test-harness-reports.ps1 -SelfTest` returned `selfTest:PASS`.
  No ASIO endpoint-output or audible playback validation was run for this
  cleanup pass.


## Status refresh: 2026-06-04 (ASIO Slice E: AsioOutputWorker extraction)

- **Structure split**: Extracted AsioOutputWorker class (1578 lines) from
  windowsasioaudioplayer.cpp (3025 lines) to new files:
  - windowsasioaudioplayer_worker.h — class definition with shared globals
  - windowsasioaudioplayer_worker.cpp — global variable definitions and MOC
- **Main file reduced**: windowsasioaudioplayer.cpp now ~1600 lines (down from 3025).
- **Key changes**:
  - ASIO callback implementations moved from anonymous namespace to file-scope
  - NoiseShaperState and related functions namespaced under AsioWorker::
  - Shared globals (g_callbackWorker, g_callbackWorkerMutex) declared extern in header
- **Validation**:
  - Build: scripts\build-app-msvc.cmd -BuildDir build-mm -Configuration Debug — PASS
  - ASIO smoke: un-playback-smoke.ps1 -Source smoke.wav -AsioOutputIndex 0 — PASS
    - loadedBackend: Windows ASIO
    - sioSelectedDescription: Creative Sound Blaster ASIO Device
    - sioBackendStartVerified: True
    - submittedPcmConclusion: ASIO submitted PCM clean
- **Evidence limits**: Endpoint acoustic verification not performed (INCONCLUSIVE).
- **Branch**: opencode-0528 (current working branch).
## Status refresh: 2026-05-24 (Codex fake-start follow-up)

- Root cause for the Apple Music occupied-device fake start: the current
  0523 ASIO worktree still had a WASAPI endpoint peak-meter override in
  `checkWasapiSessionsForEndpoint()`. If an external active session reported
  near-zero endpoint peak, the code treated the endpoint as clear and skipped
  the 15-second ASIO busy retry/status path. That can be wrong for Apple Music
  or other apps that keep the endpoint occupied while producing silence or
  near-silence.
- Fix: removed the peak-meter override. Any active external WASAPI render
  session on the mapped Creative/Sound Blaster endpoint is treated as busy, so
  the ASIO 15-second retry/status path can run instead of entering driver init.
- Fix: ASIO no longer emits `ASIO 播放已启动` immediately after queueing the
  decoder. It emits `ASIO 正在启动播放…` during startup and only emits
  `ASIO 播放已启动` after the first ASIO `bufferSwitch` callback is observed.
  The retry deadline is also cleared only after that first callback, not after
  the driver's early `ActiveState`.
- Harness/reporting update: ASIO `-RequirePlaying` now requires
  `firstBufferSwitch` when `-AsioOutputIndex` is used. The app report keeps a
  separate `playbackStateReachedPlaying` field, while ASIO `playbackStarted`
  requires the first buffer callback to avoid state-only false positives.
- Validation: direct `scripts/build-app.ps1` failed early because `cmake` was
  not on the plain PowerShell `PATH`; the documented wrapper build passed with
  `scripts/build-app-msvc.cmd -BuildDir build-codex-asio-fake-start
  -Configuration Debug -DeployFfmpegExecutable D:\Tool\ffmpeg\bin\ffmpeg.exe
  -DeployFfprobeExecutable D:\Tool\ffmpeg\bin\ffprobe.exe`. Build log:
  `build-claude-logs/build-app-msvc-470616a4032b42d9a225c5965073fa4a.log`.
- Validation: generated fixtures with `scripts/ensure-playback-fixtures.ps1
  -BuildDir build-codex-asio-fake-start -FfmpegPath
  D:\Tool\ffmpeg\bin\ffmpeg.exe`.
- Validation: ASIO smoke using Creative Sound Blaster ASIO index 0 with
  `-RequirePlaying -RejectPlaybackErrors -QuitAfterMs 24000` observed
  configure/prepare/Active/first-buffer-switch and wrote log
  `build-codex-asio-fake-start/cache/logs/player-smoke-20260524-232756-032-07c17e0e.log`
  plus harness report
  `build-codex-asio-fake-start/cache/logs/player-smoke-20260524-232756-032-07c17e0e.harness.json`.
  Harness result is `INCONCLUSIVE` because endpoint output was not verified;
  `scripts/test-harness-reports.ps1 -Path ...07c17e0e.harness.json` and
  `scripts/test-harness-reports.ps1 -SelfTest` both passed.
- Validation: simulated a silent active WASAPI shared occupier with
  `silence-48k-stereo.wav`, then ran ASIO index 0 with
  `-RequireErrorPattern 'ASIO 设备仍被其他应用占用' -QuitAfterMs 22000`.
  The ASIO log
  `build-codex-asio-fake-start/cache/logs/player-smoke-20260524-233048-231-1cfba61f.log`
  showed `active-external`, 30 retry attempts over about 15 seconds, then
  `session-check-timeout`, with no `openDriver` and no `firstBufferSwitch`.
  The app report records `playbackStateReachedPlaying=true`,
  `asioFirstBufferSwitchObserved=false`, and `playbackStarted=false`. Harness
  report
  `build-codex-asio-fake-start/cache/logs/player-smoke-20260524-233048-231-1cfba61f.harness.json`
  validated with `scripts/test-harness-reports.ps1 -Path ...1cfba61f.harness.json`.


## Status refresh: 2026-06-04 (ASIO Slice E: AsioOutputWorker extraction)

- **Structure split**: Extracted AsioOutputWorker class (1578 lines) from
  windowsasioaudioplayer.cpp (3025 lines) to new files:
  - windowsasioaudioplayer_worker.h — class definition with shared globals
  - windowsasioaudioplayer_worker.cpp — global variable definitions and MOC
- **Main file reduced**: windowsasioaudioplayer.cpp now ~1600 lines (down from 3025).
- **Key changes**:
  - ASIO callback implementations moved from anonymous namespace to file-scope
  - NoiseShaperState and related functions namespaced under AsioWorker::
  - Shared globals (g_callbackWorker, g_callbackWorkerMutex) declared extern in header
- **Validation**:
  - Build: scripts\build-app-msvc.cmd -BuildDir build-mm -Configuration Debug — PASS
  - ASIO smoke: un-playback-smoke.ps1 -Source smoke.wav -AsioOutputIndex 0 — PASS
    - loadedBackend: Windows ASIO
    - sioSelectedDescription: Creative Sound Blaster ASIO Device
    - sioBackendStartVerified: True
    - submittedPcmConclusion: ASIO submitted PCM clean
- **Evidence limits**: Endpoint acoustic verification not performed (INCONCLUSIVE).
- **Branch**: opencode-0528 (current working branch).
## Status refresh: 2026-05-24 (MiMo recovery follow-up)

- **Crash fix**: Removed audio worker/thread recreation on retry (caused crash
  from stale queued signal emissions on deleted objects). Replaced with
  `forceReleaseDriver()` — forces full ASIO driver teardown (stop, dispose
  buffers, release COM, FreeLibrary) before the next `openDriver()` attempt.
- **WASAPI endpoint session check with peak meter**: `checkWasapiSessionsForEndpoint()`
  uses `IAudioSessionManager2` to enumerate active audio sessions on the
  WASAPI endpoint mapped from the Creative ASIO driver CLSID. After session
  enumeration, checks the endpoint's `IAudioMeterInformation` peak value.
  If peak < 0.0001 (endpoint silent), active external sessions are overridden
  and ASIO init proceeds. This handles Apple Music paused state correctly —
  the session reports `AudioSessionStateActive` but produces no audio.
- **Callback watchdog recovery (6 attempts)**: When the callback watchdog fires
  (driver started but no buffer callbacks), the worker performs driver-level
  reinit (stop, dispose buffers, release COM, FreeLibrary, 300ms cooldown,
  reopen, restart). Limited to 6 recovery attempts. Based on observed behavior,
  the Creative driver needs ~6 reinit cycles to recover callbacks after
  poisoning. Watchdog timeout reduced to 1500ms, cooldown to 300ms.
- **ASIO resume buffer clear**: `resumeOutput()` now clears both ASIO
  double-buffers before calling `safeAsioStart()` to prevent stale audio data
  from playing at the start of resumed playback. Does NOT reset
  `m_renderedFrames` to avoid progress bar jumps.
- **`isLikelyCreativeDriverId` and `runDriverInitProbe` implementations added**:
  Both were declared in the header but had no implementation.
- **`resolveWasapiEndpointForAsioDriver` fixed**: Now correctly maps ASIO CLSID
  to driver name via registry lookup before checking for Creative keywords.
- Build: `build-mimo-asio\playable\Debug\20260524-071118\AudioPlayer.exe`.


## Status refresh: 2026-06-04 (ASIO Slice E: AsioOutputWorker extraction)

- **Structure split**: Extracted AsioOutputWorker class (1578 lines) from
  windowsasioaudioplayer.cpp (3025 lines) to new files:
  - windowsasioaudioplayer_worker.h — class definition with shared globals
  - windowsasioaudioplayer_worker.cpp — global variable definitions and MOC
- **Main file reduced**: windowsasioaudioplayer.cpp now ~1600 lines (down from 3025).
- **Key changes**:
  - ASIO callback implementations moved from anonymous namespace to file-scope
  - NoiseShaperState and related functions namespaced under AsioWorker::
  - Shared globals (g_callbackWorker, g_callbackWorkerMutex) declared extern in header
- **Validation**:
  - Build: scripts\build-app-msvc.cmd -BuildDir build-mm -Configuration Debug — PASS
  - ASIO smoke: un-playback-smoke.ps1 -Source smoke.wav -AsioOutputIndex 0 — PASS
    - loadedBackend: Windows ASIO
    - sioSelectedDescription: Creative Sound Blaster ASIO Device
    - sioBackendStartVerified: True
    - submittedPcmConclusion: ASIO submitted PCM clean
- **Evidence limits**: Endpoint acoustic verification not performed (INCONCLUSIVE).
- **Branch**: opencode-0528 (current working branch).
## Status refresh: 2026-05-24 (15-second busy-retry timeout)

- **Watchdog recovery changed from count-based to time-based**: Previously gave
  up after 6 fixed recovery attempts (~11 seconds). Now retries for up to 15
  seconds (`kAsioBusyRetryTimeoutMs = 15000`) with 500ms intervals between
  `openDriver()` retries (`kAsioBusyRetryIntervalMs = 500`). Uses
  `m_callbackWatchdogRecoveryStartMs` for time tracking.
- **startPipeline session check now retries (non-blocking)**: Previously aborted
  immediately when WASAPI detected an active external session. Now uses
  `QTimer::singleShot` for non-blocking retry every 500ms for up to 15 seconds.
  Status bar shows `ASIO 设备被占用，正在重试（Xs/15s）` during wait.
- **prepareOutput DeviceBusy retry**: If session check passes but `prepareOutput`
  fails with DeviceBusy (e.g. Apple Music paused, session ended but driver still
  occupied), goes back to `startPipeline` to re-check and retry. Global deadline
  (`m_sessionRetryDeadline`) covers the entire cycle.
- **Driver reset request recovery also retries**: `handleDriverResetRequest()`
  now loops `openDriver()` for up to 15 seconds instead of giving up on first
  failure.
- **Peak meter override removed**: Any active WASAPI session (including
  paused/silent apps) is now treated as occupying the device. No more peak meter
  check to override session detection.
- **UI state during retry**: initial dirty MiMo code set
  `setPlaybackState(Playing)` during busy wait to avoid a gray progress bar.
  The later Codex cleanup removed that false-playing state and keeps retry
  progress in status messages until real ASIO start evidence appears.
- **Status bar timeout display**: Shows `[elapsed]/[timeout]` format during
  retry, e.g. `ASIO 设备被占用，正在重试（3s/15s）`.
- **statusMessage signal added**: New `statusMessage(QString)` signal on
  `AudioPlayerBackend`, connected to status bar in MainWindow (18s timeout).
- **Build**: `build-mimo-asio\playable\Debug\20260524-103101\AudioPlayer.exe`.
- **Known issues**:
  - Apple Music paused → ASIO: session check may pass (session ended), then
    `prepareOutput` fails with DeviceBusy. Retry loop handles this but UX could
    be smoother.
  - Pause/resume audio jump: ASIO buffer dispose/recreate on resume flushes DMA
    caches (Creative driver retains stale audio). Decoder data loss during pause
    is now fixed (drain suspended, pipe backpressure freezes ffmpeg).
- **Status refresh: 2026-05-24 (earlier)**:
- Codex follow-up disabled the automatic ASIO-to-WASAPI-exclusive fallback in
  `MainWindow`. ASIO startup errors now remain on the Windows ASIO backend and
  log `asio fallback-to-wasapi-exclusive disabled` /
  `action=asio-fallback-disabled`; the app no longer silently switches to
  WASAPI exclusive after Creative ASIO fails. Manual output-mode switching is
  unchanged.
- The isolated ASIO process wrapper no longer marks playback as `Playing` when
  the hidden host process merely starts. It now waits for the hidden ASIO host
  to emit a real `Playing` state, so automation no longer records a false ASIO
  playback start when Creative `IASIO::init` fails immediately.
- Creative/Sound Blaster ASIO startup now checks matching active WASAPI render
  sessions before calling `IASIO::init`. The check maps the Creative ASIO driver
  to active render endpoints whose friendly name contains Creative/Sound
  Blaster/BlasterX/G5, then uses `IAudioSessionManager2` to look for active
  external sessions. If another app is still active on the Sound Blaster endpoint
  (Apple Music playing, or a test occupier), ASIO startup aborts with
  `ASIO 设备仍被其他应用占用，请暂停或关闭其他音频应用后重试` before loading/opening the
  Creative ASIO driver.
- Validation: build passed with
  `scripts/build-app.ps1 -BuildDir build-codex-asio3 -Configuration Debug
  -DeployFfmpegExecutable D:\Tool\ffmpeg\bin\ffmpeg.exe
  -DeployFfprobeExecutable D:\Tool\ffmpeg\bin\ffprobe.exe` after loading VS
  2026 `VsDevCmd.bat` and adding `D:\Qt\Tools\CMake_64\bin` to `PATH`. Latest
  playable app:
  `build-codex-asio3\playable\Debug\20260524-045447\AudioPlayer.exe`.
- Validation: Creative ASIO smoke with no external active session wrote
  `build-codex-asio3\cache\logs\player-smoke-20260524-045456-905-35529db4.log`
  and harness report
  `build-codex-asio3\cache\logs\player-smoke-20260524-045456-905-35529db4.harness.json`.
  The log shows `wasapiSessionCheck no-active-external`, then Creative isolated
  host `null-handle` init still failed with `crashed=1`; the UI logged
  `asio fallback-to-wasapi-exclusive disabled`. Smoke result remains `FAIL`
  under `-RequirePlaying` because no ASIO playback start occurred; harness schema
  validation passed with `scripts/test-harness-reports.ps1 -Path ...35529db4.harness.json`.
- Validation: occupied-endpoint smoke used a separate WASAPI shared AudioPlayer
  process as an external active session, then attempted Creative ASIO. It wrote
  `build-codex-asio3\cache\logs\player-smoke-20260524-045546-011-4c67ce44.log`
  and harness report
  `build-codex-asio3\cache\logs\player-smoke-20260524-045546-011-4c67ce44.harness.json`.
  The ASIO host logged `wasapiSessionCheck active-external ... name=扬声器
  (Sound BlasterX G5)` and `asio startPipeline aborted
  active-external-session`; no `asio openDriver` line appeared, so `IASIO::init`
  was not called while the endpoint was occupied. The UI also logged
  `action=asio-fallback-disabled`. Harness schema validation passed; the smoke
  result is expected `FAIL` because playback was intentionally blocked.
- Current limitation: the isolated host wrapper (`WindowsAsioProcessAudioPlayer`)
  causes Creative ASIO `IASIO::init` to crash in every child process, even when
  no external session is active. The non-isolated path (`WindowsAsioAudioPlayer`
  directly in the UI process) works correctly with child probes and session checks.
- **MiMo follow-up (2026-05-24) — root cause found**: The isolated ASIO host
  process wrapper (`WindowsAsioProcessAudioPlayer`) is the root cause of the
  Creative ASIO init crash. Investigation:
  1. **WASAPI shared-mode preflight, DLL unload, retry count increase**: All
     attempted in the isolated host path. None prevented the crash.
  2. **Key evidence**: Manual child probe with null-handle SUCCEEDED at
     `04:38:39` (`result=1 crashed=0`) but the isolated host with the same
     null-handle FAILS minutes later. The difference is that child probes run
     as completely independent processes via `QProcess::start()`, while the
     isolated host is a child of the UI process and may inherit COM/threading
     state that poisons the Creative driver.
  3. **Fix confirmed**: Reverting the factory to use `WindowsAsioAudioPlayer`
     directly (no isolated host wrapper) restores ASIO playback.
  Build: `build-mimo-asio\playable\Debug\20260524-053805\AudioPlayer.exe`.
  Smoke log: `build-mimo-asio\cache\logs\player-smoke-20260524-053814-249-4a11d438.log`.
  Smoke result: PASS (`asioBackendStartVerified=True`,
  `asioFirstBufferSwitchObserved=True`, `maxPosition=11050`).
  **Fix**: Revert `audioplayerfactory.cpp` to return `WindowsAsioAudioPlayer`
  instead of `WindowsAsioProcessAudioPlayer`. The non-isolated path already has
  session checks, child probes, WASAPI exclusive preflight, callback watchdog,
  and `kAsioResetRequest` handling.


## Status refresh: 2026-06-04 (ASIO Slice E: AsioOutputWorker extraction)

- **Structure split**: Extracted AsioOutputWorker class (1578 lines) from
  windowsasioaudioplayer.cpp (3025 lines) to new files:
  - windowsasioaudioplayer_worker.h — class definition with shared globals
  - windowsasioaudioplayer_worker.cpp — global variable definitions and MOC
- **Main file reduced**: windowsasioaudioplayer.cpp now ~1600 lines (down from 3025).
- **Key changes**:
  - ASIO callback implementations moved from anonymous namespace to file-scope
  - NoiseShaperState and related functions namespaced under AsioWorker::
  - Shared globals (g_callbackWorker, g_callbackWorkerMutex) declared extern in header
- **Validation**:
  - Build: scripts\build-app-msvc.cmd -BuildDir build-mm -Configuration Debug — PASS
  - ASIO smoke: un-playback-smoke.ps1 -Source smoke.wav -AsioOutputIndex 0 — PASS
    - loadedBackend: Windows ASIO
    - sioSelectedDescription: Creative Sound Blaster ASIO Device
    - sioBackendStartVerified: True
    - submittedPcmConclusion: ASIO submitted PCM clean
- **Evidence limits**: Endpoint acoustic verification not performed (INCONCLUSIVE).
- **Branch**: opencode-0528 (current working branch).
## Status refresh: 2026-05-23

- Codex follow-up for Creative Sound Blaster ASIO same-process recovery:
  `WindowsAsioAudioPlayer` now resets the ASIO worker after ASIO output-start
  failure paths, so a callback-watchdog/device-busy failure tears down the
  pipeline, releases ASIO output resources, then recreates the worker thread and
  COM apartment before the next user retry. `AsioOutputWorker::openDriver()` now
  clears stale open-failure state before each attempt, performs COM
  `CoFreeUnusedLibraries()`/`CoUninitialize()` cleanup on failed open attempts,
  and records `asio openDriver cleanup-after-failure`.
- Main-window ASIO playback now uses an isolated host process. The factory
  returns `WindowsAsioProcessAudioPlayer` for the Windows ASIO backend; that
  wrapper starts the current `AudioPlayer.exe` with hidden `--asio-host-play`
  arguments and receives JSON events from stdout. The hidden host process uses
  the existing in-process `WindowsAsioAudioPlayer`, so Creative's ASIO DLL is no
  longer loaded by the UI process during normal ASIO playback. Pause/seek/volume
  changes currently restart the host at the tracked position rather than sending
  live ASIO commands.
- Creative/Sound Blaster ASIO keeps the normal host order (`app-window`,
  `desktop`, `null-handle`; `preferNullHost=0`) but now shields `IASIO::init`
  attempts with a short-lived child process. The parent runs one child probe per
  host candidate and only calls `init` in the main app process after a child
  confirms that host can initialize. A failed or crashing Creative init therefore
  does not poison the current AudioPlayer process; the next user click can probe
  again after Apple Music or another app releases the device.
- Creative/Sound Blaster ASIO also runs a short WASAPI exclusive preflight before
  the child probes. On this machine the default Sound BlasterX G5 endpoint
  `{0.0.0.00000000}.{67bad92e-2d15-4e47-9aba-e09f5299c8d1}` rejected its
  192 kHz mix format for exclusive mode but accepted and started the fallback
  `48000 Hz, 2ch, 16-bit PCM` probe. This matches the user's observation that
  WASAPI exclusive can take the endpoint, but it does not by itself guarantee
  Creative ASIO will initialize.
- For the 2026-05-23 build, when Creative ASIO still could not initialize after
  all child probes failed, the UI automatically fell back from ASIO to WASAPI
  exclusive mode for the same source and attempted to select the matching
  Creative/Sound Blaster endpoint. That was a usability fallback, not proof that
  the Creative ASIO driver recovered.
- The ASIO callback watchdog now marks a host that reaches `Active` without a
  first buffer callback and retries another host in the same playback session
  before surfacing the error. This covers the observed Creative "fake start"
  state where `start()` returns success but no `firstBufferSwitch` arrives.
- Validation: build passed with
  `scripts/build-app.ps1 -BuildDir build-codex-asio3 -Configuration Debug
  -QtPrefix D:\Qt\6.11.1\msvc2022_64 -DeployFfmpegExecutable
  D:\Tool\ffmpeg\bin\ffmpeg.exe -DeployFfprobeExecutable
  D:\Tool\ffmpeg\bin\ffprobe.exe` after loading VS 2026 vcvars and adding
  `D:\Qt\Tools\CMake_64\bin;D:\Qt\Tools\Ninja` to `PATH`. The latest playable
  app for this investigation was
  `build-codex-asio3\playable\Debug\20260523-113938\AudioPlayer.exe`.
- Validation: device-list smoke using
  `scripts/run-playback-smoke.ps1 -BuildDir build-codex-asio3 -Configuration
  Debug -Source build-codex-asio3\fixtures\sine-1khz-minus18db-48k-stereo.wav
  -ListOutputDevices -QuitAfterMs 4000 -FfmpegPathOverride
  D:\Tool\ffmpeg\bin\ffmpeg.exe -FfprobePathOverride
  D:\Tool\ffmpeg\bin\ffprobe.exe` completed with `harnessResult=PASS` in
  `build-codex-asio3\cache\logs\player-smoke-20260523-111136-669-d8d66bc6.harness.json`
  and listed one ASIO output:
  `asioOutputDevice index=0 description=Creative Sound Blaster ASIO Device`.
  `scripts/test-harness-reports.ps1 -Path build-codex-asio3\cache\logs\player-smoke-20260523-111136-669-d8d66bc6.harness.json`
  passed schema validation.
- Validation: Creative ASIO smoke using `-AsioOutputIndex 0 -RequirePlaying
  -RejectPlaybackErrors` wrote harness report
  `build-codex-asio3\cache\logs\player-smoke-20260523-111057-815-34a04272.harness.json`
  and resulted `FAIL`. Evidence: ASIO selection/configure were observed; WASAPI
  exclusive preflight succeeded; all three child probes logged
  `asio childProbe init ... result=0 crashed=1`; the parent logged
  `asio openDriver childProbe failed` and did not call `IASIO::init` in the main
  process. The app emitted
  `ASIO 设备被占用或驱动暂不可用，请暂停或关闭其他音频应用后重试`.
  `scripts/test-harness-reports.ps1
  -Path build-codex-asio3\cache\logs\player-smoke-20260523-111057-815-34a04272.harness.json`
  passed schema validation.
- Validation: Creative ASIO fallback smoke using `-AsioOutputIndex 0
  -RequirePlaying -RejectPlaybackErrors` wrote harness report
  `build-codex-asio3\cache\logs\player-smoke-20260523-114006-507-180d8e67.harness.json`
  and text log
  `build-codex-asio3\cache\logs\player-smoke-20260523-114006-507-180d8e67.log`.
  The UI process logged `asio-process host-start`, the hidden ASIO host process
  configured Creative Sound Blaster ASIO, and the host-side child probes still
  all reported `crashed=1`. The host emitted the ASIO-busy error, then the UI
  logged `action=asio-fallback-wasapi-exclusive`, switched to
  `扬声器 (Sound BlasterX G5)`, initialized WASAPI exclusive at 192 kHz/2ch/24-bit,
  and reached `state=Playing`. A post-smoke process check found no remaining
  `AudioPlayer` host processes. The smoke result remains `FAIL` under
  `-RejectPlaybackErrors` because the original ASIO error is still intentionally
  present in the log; treat the fallback playback observation as functional
  evidence and the true ASIO start result as `FAIL`.
  `scripts/test-harness-reports.ps1
  -Path build-codex-asio3\cache\logs\player-smoke-20260523-114006-507-180d8e67.harness.json`
  passed schema validation.
- Current limitation: the process-host wrapper gives ASIO playback a restartable
  process boundary, but Creative ASIO playback is still not locally proven on
  this machine. Under the current machine state, Creative ASIO `init` crashes
  even in a fresh hidden host and in that host's child probes after a successful
  WASAPI exclusive preflight. Treat the current result as `FAIL` for Creative
  ASIO endpoint/start, not as endpoint-output validation. At that point, the
  automatic WASAPI-exclusive fallback was still the non-restart mitigation.
- Next priority: manually reproduce the user scenario with the updated build.
  If ASIO recovers after pausing the external app, the expected log sequence is
  `asio-process host-start`, then host-side `asio openDriver childProbe ... ok=1`,
  `asio openDriver init succeeded`, and `firstBufferSwitch`. If every fresh host
  still logs child-probe `crashed=1`, the remaining blockage is outside both the
  UI process and the restartable ASIO host process and likely needs a Creative
  driver reset/replug/service-level workaround.

- ASIO submitted-output evidence is now captured. `AsioOutputWorker::renderCallback()`
  runs `AudioArtifactMonitor::analyzePcmBlock()` on every submitted buffer, using
  the source PCM chunk before ASIO sample-type conversion. `finishOutput()` logs
  `asioRenderMirrorConclusion` with `artifactDetected`, `artifactCount`, and
  `capturedBlocks` fields. `PlayerLogger::diagnostic()` emits a structured
  `asio_render_mirror_conclusion` JSONL event with `observationLayer` set to
  `"ASIO submitted PCM before endpoint output"`.
- The app diagnostic report (`src/ui/main.cpp`) now includes `asioRenderMirrorObserved`,
  `asioRenderMirrorClean`, and `asioRenderMirrorArtifactDetected` fields. The
  `submittedPcmConclusion` and `popClickVerification` fields consider ASIO evidence
  alongside WASAPI evidence.
- The smoke script (`scripts/run-playback-smoke.ps1`) reads the new ASIO app report
  fields and sets `backendEvidence.submittedOutputVerified` accordingly. The ASIO
  `scope` now includes `"asio-selection-start-submitted-output-and-driver-callback"`
  when submitted output is verified. The `submittedOutputEvidenceLayer` is set to
  `"asio-submitted-pcm-artifact-monitor"` when evidence exists.
- `scripts/test-harness-reports.ps1 -SelfTest` fixture updated to reflect ASIO
  submitted-output verification capability.
- **kAsioResetRequest handling**: The `asioMessage` callback now signals
  `AsioOutputWorker` when the ASIO driver sends `kAsioResetRequest`. The worker
  sets an atomic flag (`m_resetRequestCount`) which is checked on the worker
  thread's position timer (100ms interval). When detected, the worker performs
  a driver-level reinit: stops ASIO streaming, disposes buffers, releases the
  COM driver object and unloads the DLL, waits for cooldown, then reopens the
  driver via `openDriver()` and restarts streaming. This preserves the decoder
  session and PCM buffer — only the ASIO driver is reinitialized. If reinit
  fails, the worker emits `stateChanged(StoppedState, OpenError)` which triggers
  `handleAudioStateChanged` to tear down the pipeline. The reinit path is
  untested because no ASIO device was available during validation; the build
  compiles and the existing smoke path (device-occupied detection) works correctly.
- Endpoint-output evidence remains out of scope; `endpointOutputVerified` is always
  `false` for ASIO runs.
- Device-occupied detection: `isAudioEndpointBusy()` probes the audio endpoint via
  WASAPI `IAudioClient` shared-mode before ASIO playback. A 2-second callback
  watchdog in `startOutput()` detects when the ASIO driver starts but doesn't fire
  `firstBufferSwitch`. Both paths emit `errorOccurred` with user-facing messages.
- Output device/backend selection is now persisted to QSettings
  (`player/outputBackend`, `player/outputDeviceId`) and restored on startup.
- Shared mode is always the default on startup, regardless of saved exclusive setting.

## Known unresolved issues (2026-05-30)

- **WASAPI probe limitations**: `isAudioEndpointBusy()` only detects WASAPI-level
  device occupation. DirectSound or Kernel Streaming occupation is not detected by
  this probe, but ASIO `init` failure handling covers those cases.

## Recently resolved issues

- **Creative ASIO hot-recovery [RESOLVED 2026-05-30]**: ASIO hot-switch recovery
  now works. When another WASAPI session occupies the same endpoint, ASIO detects
  via `checkWasapiSessionsForEndpoint()` and retries. After occupier stops, ASIO
  successfully initializes without app restart. Validated with
  `scripts\test-asio-hot-switch.ps1`.
- **Creative ASIO exit crash [RESOLVED 2026-05-30]**: Process now exits cleanly
  (exit code 0) after ASIO playback. All ASIO driver calls protected with SEH.
  No longer reproducible.


## Status refresh: 2026-06-04 (ASIO Slice E: AsioOutputWorker extraction)

- **Structure split**: Extracted AsioOutputWorker class (1578 lines) from
  windowsasioaudioplayer.cpp (3025 lines) to new files:
  - windowsasioaudioplayer_worker.h — class definition with shared globals
  - windowsasioaudioplayer_worker.cpp — global variable definitions and MOC
- **Main file reduced**: windowsasioaudioplayer.cpp now ~1600 lines (down from 3025).
- **Key changes**:
  - ASIO callback implementations moved from anonymous namespace to file-scope
  - NoiseShaperState and related functions namespaced under AsioWorker::
  - Shared globals (g_callbackWorker, g_callbackWorkerMutex) declared extern in header
- **Validation**:
  - Build: scripts\build-app-msvc.cmd -BuildDir build-mm -Configuration Debug — PASS
  - ASIO smoke: un-playback-smoke.ps1 -Source smoke.wav -AsioOutputIndex 0 — PASS
    - loadedBackend: Windows ASIO
    - sioSelectedDescription: Creative Sound Blaster ASIO Device
    - sioBackendStartVerified: True
    - submittedPcmConclusion: ASIO submitted PCM clean
- **Evidence limits**: Endpoint acoustic verification not performed (INCONCLUSIVE).
- **Branch**: opencode-0528 (current working branch).
## Status refresh: 2026-05-21

- Smoke harness reports now use `schemaVersion=2` with `backendEvidence` for
  ASIO selection/start evidence. For an ASIO run, the harness records selected
  driver details, configure-output fields, active sample rate, preferred buffer
  size, prepare-output observation, active audio state observation, first buffer
  switch observation, submitted-output verification status, and endpoint-output
  verification status.
- Validation: `scripts/test-harness-reports.ps1 -SelfTest` passed; ASIO smoke
  using `-ListOutputDevices -AsioOutputIndex 1 -RequirePlaying
  -RejectPlaybackErrors` selected `VB-Matrix VASIO-128`, loaded `Windows ASIO`,
  observed configure/prepare/Active/first-buffer-switch evidence, and wrote
  harness report
  `build-mm/cache/logs/player-smoke-20260521-053703-461-bca36cb6.harness.json`.
  The run reported `asioBackendStartVerified=True`, configured rate `48000`,
  active sample rate `44100`, preferred buffer size `512`, and
  `harnessResult=INCONCLUSIVE`.
- The remaining ASIO automation gap is no longer selection/start evidence
  semantics. The remaining gap is ASIO-specific submitted-output evidence and
  endpoint-output evidence; do not treat this smoke result as ASIO stability or
  endpoint-output validation.

- `scripts/run-playback-smoke.ps1` now exposes `-AsioOutputIndex <n>`, routed
  to the app CLI's existing `--asio-output-index <n>` option.
- `-AsioOutputIndex` cannot be combined with the system-output
  `-OutputDeviceIndex` option in the smoke wrapper.
- Smoke harness reports now record ASIO selection request and observation
  fields: requested ASIO index, whether selection was confirmed, selected ASIO
  index/id/description, ASIO device count when listed, and the backend loaded
  for the source.
- When `-AsioOutputIndex` is supplied, the smoke wrapper fails the run if the
  app does not log `action=select-asio-output-device`, if the confirmed index
  does not match, or if the source load does not use an ASIO backend.
- Validation: `scripts/test-harness-reports.ps1 -SelfTest` passed;
  non-ASIO smoke using `-ListOutputDevices`, `-RequirePlaying`, and
  `-RejectPlaybackErrors` passed with harness report
  `build-mm/cache/logs/player-smoke-20260521-051622-326-f6554bda.harness.json`;
  ASIO smoke using `-AsioOutputIndex 1` selected `VB-Matrix VASIO-128`, loaded
  `Windows ASIO`, satisfied `-RequirePlaying`, and wrote harness report
  `build-mm/cache/logs/player-smoke-20260521-051722-206-48e1000e.harness.json`.
  `scripts/test-harness-reports.ps1 -LatestSmoke` passed for that latest ASIO
  report.
- The ASIO smoke result was `INCONCLUSIVE`, not `PASS`, because the current
  app/harness pop and submitted-PCM conclusions are still WASAPI-oriented and
  endpoint output was not verified. This is useful ASIO selection/start
  evidence, not a broad ASIO stability or endpoint-output claim.


## Status refresh: 2026-06-04 (ASIO Slice E: AsioOutputWorker extraction)

- **Structure split**: Extracted AsioOutputWorker class (1578 lines) from
  windowsasioaudioplayer.cpp (3025 lines) to new files:
  - windowsasioaudioplayer_worker.h — class definition with shared globals
  - windowsasioaudioplayer_worker.cpp — global variable definitions and MOC
- **Main file reduced**: windowsasioaudioplayer.cpp now ~1600 lines (down from 3025).
- **Key changes**:
  - ASIO callback implementations moved from anonymous namespace to file-scope
  - NoiseShaperState and related functions namespaced under AsioWorker::
  - Shared globals (g_callbackWorker, g_callbackWorkerMutex) declared extern in header
- **Validation**:
  - Build: scripts\build-app-msvc.cmd -BuildDir build-mm -Configuration Debug — PASS
  - ASIO smoke: un-playback-smoke.ps1 -Source smoke.wav -AsioOutputIndex 0 — PASS
    - loadedBackend: Windows ASIO
    - sioSelectedDescription: Creative Sound Blaster ASIO Device
    - sioBackendStartVerified: True
    - submittedPcmConclusion: ASIO submitted PCM clean
- **Evidence limits**: Endpoint acoustic verification not performed (INCONCLUSIVE).
- **Branch**: opencode-0528 (current working branch).
## Status refresh: 2026-05-19

- This tracker is split out from the WASAPI anomaly file so ASIO work has its
  own evidence trail.
- No active ASIO regression is consolidated in this file yet.
- Treat ASIO findings separately from WASAPI endpoint, APO, spatial-audio, and
  loopback conclusions unless logs point to shared source preparation, factory,
  UI, cache, or harness behavior.
- The app CLI and smoke wrapper both have an ASIO driver selection option. The
  current remaining automation gap is ASIO-specific submitted-output evidence
  and endpoint-output evidence beyond selection/start/driver-callback
  assertions.

## Current focus

- Use this file for ASIO device enumeration, driver selection, start/stop,
  source switching, recovery, packaging, and driver-specific playback issues.
- Capture the exact ASIO driver, selected index, source file, sample rate,
  buffer settings when visible, command, report path, and log path for each
  reproduced issue.
- Keep ASIO code changes localized to `src/backends/asio/` unless the evidence
  points to a shared layer.

## Current priority order

1. Use `scripts/run-playback-smoke.ps1 -AsioOutputIndex <n>` for requested
   single-driver ASIO smoke validation, and keep exact driver evidence in the
   harness report.
2. ASIO submitted-output evidence is now captured via `AudioArtifactMonitor`
   in the render callback. Use this as the submitted-PCM validation baseline.
3. Keep ASIO release/package findings separate from WASAPI playback findings.

## Current validation baseline

- Use `scripts/build-app.ps1` for build validation after ASIO code changes.
- Use the checked-in smoke harness where it can express the scenario; otherwise
  record the exact app CLI command and generated report/log paths.
- For ASIO selection/start validation, use `-AsioOutputIndex <n>` with
  `-RequirePlaying` and record the selected ASIO index/id/description from the
  harness report.
- Do not claim ASIO stability from WASAPI-only smoke, loopback, or manual
  spatial-audio evidence.
- Do not promote ASIO smoke `INCONCLUSIVE` to `PASS` until ASIO-specific
  submitted-output evidence exists and endpoint-output limitations are explicit.

## Current acceptance bar

- ASIO findings name the driver, source, command, evidence files, and whether
  the issue is backend-specific or shared.
- ASIO fixes are validated on an ASIO path, or the remaining gap is explicitly
  labeled as unvalidated.
- ASIO smoke can reach `PASS` when submitted-output evidence is clean and no
  other fail conditions are present. Endpoint-output limitations are still
  explicit in the harness report.

## Dated notes

- 2026-05-19: Created this tracker. No active ASIO issue is consolidated here
  yet.
- 2026-05-21: Added smoke wrapper ASIO selection support and validated selection
  to `VB-Matrix VASIO-128` with `-AsioOutputIndex 1`. Result was
  `INCONCLUSIVE` because only ASIO selection/start was verified.
- 2026-05-21: Added ASIO v2 `backendEvidence` report semantics. Latest ASIO smoke
  verified selection/start/first-buffer-switch evidence but remains
  `INCONCLUSIVE` because submitted-output and endpoint-output evidence are not
  captured.
- 2026-05-23: Added ASIO submitted-output evidence via `AudioArtifactMonitor` in
  `renderCallback()`. App report now includes `asioRenderMirror*` fields. Smoke
  script reads ASIO evidence and sets `backendEvidence.submittedOutputVerified`.
  ASIO smoke can now reach `PASS` when submitted PCM is clean. Endpoint-output
  evidence remains out of scope.
- 2026-05-23: Added `kAsioResetRequest` handling. The `asioMessage` callback now
  signals the worker via `notifyResetRequest()` (atomic flag). The worker's
  position timer detects the flag and performs driver-level reinit (stop, dispose
  buffers, release COM, reopen, restart). Preserves decoder session and PCM buffer.
  Build compiles; existing smoke (device-occupied) passes. Reinit path untested —
  no free ASIO device available. Skipped `QMediaDevices::audioOutputsChanged`
  listener because ASIO devices are registry-based and not detected by that signal.
- 2026-05-24: Fixed pause→resume audio data loss. Previously the ffmpeg decoder
  kept running during ASIO pause, filling the 3-second PcmStreamBuffer until it
  overflowed and silently discarded new data. Now `FfmpegDecoderWorker::setPaused()`
  suspends stdout draining during pause; the OS pipe buffer (~64KB) fills and
  ffmpeg naturally blocks on `write()`. On resume, drain is kicked once to consume
  buffered pipe data, then normal signal-driven draining resumes. Ring buffer data
  is fully preserved — no discontinuity at the resume point. Build:
  `build-mimo-asio\playable\Debug\20260524-084707\AudioPlayer.exe`.

## Bit-depth precision output: 2026-05-30

- **Change**: `selectOutputFormat()` now prefers source-matching sample format
  when `m_sourceBitDepth` is known. 16-bit source → Int16 preference; 24-bit
  source → Int32 preference (Qt has no Int24). Logged as
  `asio source-bit-depth-preference`.
- **Change**: Added 2nd-order LNS (Lipshitz noise shaping) for 32→24 and 32→16
  bit conversions in `writeChannel()`. Noise shaping activates when
  `m_sourceBitDepth > 0 && m_sourceBitDepth < 32`. Per-channel state persists
  across render callbacks. Reset on `configureOutput()` / driver reopen.
- **Change**: `configureOutput()` now accepts `int sourceBitDepth` parameter,
  stored on `AsioOutputWorker` and passed from `WindowsAsioAudioPlayer`.
- **Change**: Added `NoiseShaperState` struct and `noiseShapedQuantize32()`
  function in the ASIO anonymous namespace.
- **Logging**: `openDriver()` logs `asio noiseShaping enabled` when noise shaping
  activates.
- **Limitation**: ASIO has no bit-depth probe API. Driver native type is only
  known after `createBuffers()` + `getChannelInfo()`. Noise shaping applies to
  Int24LSB, Int32LSB24, and Int16LSB output types when source bit depth < 32.
  Float32LSB output is unaffected.
- **Files changed**: `windowsasioaudioplayer.h`, `windowsasioaudioplayer.cpp`,
  `audioplayerbackend.h`, `mainwindow_media.cpp`, `mainwindow_output.cpp`.

## 32→16 noise shaping (Phase 2b): 2026-05-31

- **Change**: `AsioOutputWorker` now has `m_decoderFormat` (QAudioFormat) separate
  from `m_outputFormat`. Set in `configureOutput()`, cleared when output is
  released.
- **Change**: `normalizedSample()` uses `m_decoderFormat` to interpret buffer
  bytes. When decoder outputs s32, reads 4-byte samples correctly even when
  `m_outputFormat` is Int16.
- **Change**: `continueStartPipeline()` builds `decoderPcmFormat` that upgrades
  Int16→Int32 when `m_sourceBitDepth ≤ 16`. Builds `decoderQFormat` for the
  worker and ffmpeg CLI arguments.
- **Change**: `configureOutput()` accepts optional `decoderFormat` parameter.
  Log includes decoder format sample format.
- **Change**: ffmpeg CLI args (`-acodec`, `-f`, `aformat`) and libav in-process
  `startDecoding()` both use the upgraded decoder format (s32).
- **Files changed**: `windowsasioaudioplayer.cpp`.
