# ASIO backend code map

Read-only function/class index for
`src/backends/asio/windowsasioaudioplayer.cpp` (3797 lines). Use this map to
choose first files and functions before opening the full source. Keep durable
workflow rules in `AGENTS.md` and `docs/dev/*.md`.

## File structure

```
Lines 1-51:      Includes (project, Qt, C++, Windows/COM)
Lines 46-51:     Forward declarations / static data
Lines 53-181:    Anonymous namespace constants, types, enums
Lines 93-186:    Struct declarations
Lines 152-176:   IASIO interface (COM-style abstract)
Lines 188-1139:  Anonymous namespace free functions
Lines 1141-2491: AsioOutputWorker class
Lines 2493-2544: ASIO callback implementations (anonymous namespace)
Lines 2548-3797: WindowsAsioAudioPlayer class methods
Line 3797:       #include "windowsasioaudioplayer.moc"
```

## Namespace-scope constants and types

### ASIO protocol constants (lines 55-75)

| Line | Name | Value | Purpose |
|------|------|-------|---------|
| 55 | `kFfmpegPathOverrideEnv` | `"AUDIOPLAYER_FFMPEG_PATH"` | ffmpeg path override |
| 56 | `kAsioOk` | `0` | ASIO success code |
| 57 | `kAsioSuccess` | `0x3f4847a0` | ASIO success code |
| 58 | `kAsioNotPresent` | `-1000` | ASIO not found |
| 59 | `kAsioFalse` | `0` | ASIO boolean false |
| 60 | `kAsioTrue` | `1` | ASIO boolean true |
| 61-68 | `kAsioSelectorSupported`..`kAsioSupportsTimeCode` | `1`-`8` | Message selectors |
| 69 | `kAsioOpenRetryCount` | `5` | Driver open retry limit |
| 70 | `kAsioOpenRetryBaseDelayMs` | `160` | Retry base delay |
| 71 | `kAsioReleaseCooldownMs` | `300` | Release cooldown |
| 72 | `kAsioBusyRetryTimeoutMs` | `15000` | Session retry timeout |
| 73 | `kAsioBusyRetryIntervalMs` | `500` | Session retry interval |
| 74 | `kAsioFfmpegInitialBurstSeconds` | `1.5` | Decoder burst config |
| 75 | `g_asioHostWindowHandle` | `std::atomic<quintptr>` | Global host window |

### Type aliases (lines 77-80)

`ASIOBool`, `ASIOError`, `ASIOSampleRate`, `ASIOSampleType` — all `long`-based.

### Enum: `AsioSampleType` (lines 82-91)

Values: `ASIOSTInt16LSB(16)`, `ASIOSTInt24LSB(17)`, `ASIOSTInt32LSB(18)`,
`ASIOSTFloat32LSB(19)`, `ASIOSTInt32LSB16(24)`, `ASIOSTInt32LSB18(25)`,
`ASIOSTInt32LSB20(26)`, `ASIOSTInt32LSB24(27)`.

### Struct declarations (lines 93-186)

| Lines | Struct | Purpose |
|-------|--------|---------|
| 93-96 | `ASIOSamples` | Hi/lo 64-bit sample position |
| 100-106 | `ASIOClockSource` | Clock source descriptor |
| 108-115 | `ASIOChannelInfo` | Channel info (type, group, name) |
| 117-121 | `ASIOBufferInfo` | Double-buffer pointer pair per channel |
| 123-130 | `ASIOTimeInfo` | Time info (speed, position, rate, flags) |
| 132-137 | `ASIOTimeCode` | SMPTE time code |
| 139-143 | `ASIOTime` | Combined time info + time code |
| 145-150 | `ASIOCallbacks` | Callback vtable |
| 178-181 | `AsioDriverEntry` | Registry driver name + CLSID text pair |
| 183-186 | `AsioHostWindowCandidate` | Host window name + HWND pair |
| 336-339 | `AsioHostWindowSearch` | Transient struct for EnumWindows callback |
| 1006-1015 | `WasapiSessionCheckResult` | Session probe result |

## IASIO interface (lines 152-176)

Pure COM-style abstract interface for ASIO driver. Inherits `IUnknown`. All
methods are pure virtual: `init`, `getDriverName`, `getDriverVersion`,
`getErrorMessage`, `start`, `stop`, `getChannels`, `getLatencies`,
`getBufferSize`, `canSampleRate`, `getSampleRate`, `setSampleRate`,
`getClockSources`, `setClockSource`, `getSamplePosition`, `getChannelInfo`,
`createBuffers`, `disposeBuffers`, `controlPanel`, `future`, `outputReady`.

## Free functions by category

### Driver discovery (lines 250-449, 456-471, 2548-2616, 2618-2654)

Registry enumeration, CLSID parsing, host window discovery, COM instantiation,
SEH-wrapped init/release.

| Lines | Function | Dependency |
|-------|----------|------------|
| 250-275 | `utf16StringFromRegistryValue(HKEY, const wchar_t*)` | pure helper |
| 277-314 | `appendAsioRegistryEntries(HKEY, QList<AsioDriverEntry>*)` | pure helper |
| 316-334 | `registeredAsioDrivers()` | pure helper |
| 341-351 | `enumAsioHostWindow(HWND, LPARAM)` — Win32 callback | reads global state |
| 353-378 | `asioHostWindow()` | reads global state |
| 388-409 | `asioHostWindowCandidates()` | reads global state |
| 411-415 | `parseClsid(const QString&, CLSID*)` | pure helper |
| 417-439 | `createAsioDriver(const QString&)` | pure helper |
| 456-469 | `safeAsioInit(IASIO*, HWND, bool*)` | pure helper (SEH) |
| 471-480 | `safeAsioRelease(IASIO*)` | pure helper (SEH) |
| 2548-2551 | `WindowsAsioAudioPlayer::setHostWindowHandle(quintptr)` | reads global state |
| 2553-2599 | `WindowsAsioAudioPlayer::availableAsioOutputDevices()` | pure helper (cached) |
| 2601-2604 | `WindowsAsioAudioPlayer::hasAvailableAsioOutputDevices()` | reads player state |
| 2618-2654 | `WindowsAsioAudioPlayer::runDriverInitProbe(...)` | pure helper |

### Sample-rate / format negotiation (lines 512-812, 2012-2290, 3022-3056, 3753-3795)

Rate queries, channel/latency/buffer probing, format helpers, PCM conversion.

| Lines | Function | Dependency |
|-------|----------|------------|
| 512-525 | `safeAsioCanSampleRate(IASIO*, ASIOSampleRate, bool*)` | pure helper |
| 527-540 | `safeAsioSetSampleRate(IASIO*, ASIOSampleRate, bool*)` | pure helper |
| 542-555 | `safeAsioGetSampleRate(IASIO*, ASIOSampleRate*, bool*)` | pure helper |
| 557-570 | `safeAsioGetChannels(IASIO*, long*, long*, bool*)` | pure helper |
| 572-585 | `safeAsioGetLatencies(IASIO*, long*, long*, bool*)` | pure helper |
| 587-606 | `safeAsioGetBufferSize(IASIO*, long*, long*, long*, long*, bool*)` | pure helper |
| 608-621 | `safeAsioGetChannelInfo(IASIO*, ASIOChannelInfo*, bool*)` | pure helper |
| 674-684 | `fallbackAsioProbeFormat(const AsioDriverEntry&)` | pure helper |
| 686-702 | `pcmCodecName(QAudioFormat::SampleFormat)` | pure helper |
| 704-720 | `pcmSampleFormatName(QAudioFormat::SampleFormat)` | pure helper |
| 722-738 | `pcmMuxerName(QAudioFormat::SampleFormat)` | pure helper |
| 740-767 | `pcmStreamFormatFromQAudioFormat(const QAudioFormat&)` | pure helper |
| 769-775 | `appendUniqueSampleRate(QList<int>*, int)` | pure helper |
| 777-812 | `sourcePreferredSampleRateCandidates(int, int)` | pure helper |
| 2012-2290 | `AsioOutputWorker::openDriver()` | touches worker state |
| 3022-3036 | `WindowsAsioAudioPlayer::channelLayoutForCount(int) const` | pure helper |
| 3753-3795 | `WindowsAsioAudioPlayer::selectOutputFormat(...)` | reads player state |

### Session probing (lines 867-1137, 2005-2010, 2606-2616)

Creative driver detection, WASAPI endpoint resolution, external session checks.

| Lines | Function | Dependency |
|-------|----------|------------|
| 867-872 | `isCreativeAsioDriver(const QString&)` | pure helper |
| 874-954 | `resolveWasapiEndpointForAsioDriver(const QByteArray&)` | pure helper (COM local) |
| 956-1004 | `isAudioEndpointBusy(const QByteArray&)` | pure helper (COM local) |
| 1017-1102 | `checkWasapiSessionsForEndpoint(const QByteArray&)` | pure helper (COM local) |
| 1104-1127 | `hasExternalWasapiRenderSessionsForAsioDriver(const QByteArray&, bool)` | pure helper |
| 1129-1132 | `hasActiveExternalWasapiRenderSessionsForAsioDriver(const QByteArray&)` | pure helper |
| 1134-1137 | `hasAnyExternalWasapiRenderSessionsForAsioDriver(const QByteArray&)` | pure helper |
| 2005-2010 | `AsioOutputWorker::classifyEndpointOpenFailure() const` | reads worker state |
| 2606-2616 | `WindowsAsioAudioPlayer::isLikelyCreativeDriverId(const QString&)` | pure helper |

### Utility / state name helpers (lines 188-248, 441-454, 482-672, 814-866)

State name helpers, environment config, audio level analysis, SEH-wrapped
ASIO call wrappers.

| Lines | Function | Dependency |
|-------|----------|------------|
| 188-202 | `playbackStateName(AudioPlayerBackend::PlaybackState)` | pure helper |
| 204-218 | `audioStateName(QAudio::State)` | pure helper |
| 220-232 | `audioSessionStateName(AudioSessionState)` | pure helper |
| 234-248 | `toolExecutableOverride(const QString&)` | pure helper |
| 380-386 | `hwndText(HWND)` | pure helper |
| 441-449 | `asioDriverError(IASIO*)` | pure helper |
| 451-454 | `asioResultOk(ASIOError)` | pure helper |
| 482-495 | `safeAsioStart(IASIO*, bool*)` | pure helper |
| 497-510 | `safeAsioStop(IASIO*, bool*)` | pure helper |
| 623-642 | `safeAsioCreateBuffers(...)` | pure helper |
| 644-657 | `safeAsioDisposeBuffers(IASIO*, bool*)` | pure helper |
| 659-672 | `safeAsioOutputReady(IASIO*, bool*)` | pure helper |
| 814-840 | `sampleMagnitude(const char*, QAudioFormat::SampleFormat)` | pure helper |
| 842-857 | `boundedEnvInt(const QString&, int, int, int)` | pure helper |

### ASIO callback globals (lines 859-866, 2493-2544)

| Lines | Declaration | Dependency |
|-------|-------------|------------|
| 859 | `g_callbackWorker` — global `AsioOutputWorker*` | touches worker state via global |
| 860 | `g_callbackWorkerMutex` — global `QMutex` | touches worker state via global |
| 2495-2501 | `asioBufferSwitch(long, ASIOBool)` | touches worker state via global |
| 2503-2507 | `asioSampleRateDidChange(ASIOSampleRate)` | pure (log only) |
| 2509-2538 | `asioMessage(long, long, void*, double*)` | touches worker state via global |
| 2540-2544 | `asioBufferSwitchTimeInfo(ASIOTime*, long, ASIOBool)` | touches worker state via global |

## AsioOutputWorker class (lines 1141-2491)

Declared in the `.cpp` file with `Q_OBJECT`. MOC include at line 3797.

### Enum (line 1146)

`DriverOpenFailureReason`: `None`, `DeviceBusy`, `DriverError`, `RecoveryTimeout`

### Member data (lines 2458-2490)

Key members: `m_driver` (IASIO\*), `m_buffer` (PcmStreamBuffer\*),
`m_bufferInfos`, `m_channelInfos`, `m_driverId`, `m_outputFormat`,
`m_volume`, `m_sessionId`, `m_bufferSize`, `m_started`, `m_buffersCreated`,
`m_comInitialized`, `m_lastReleaseTimer`, `m_callbackCount` (atomic),
`m_resetRequestCount` (atomic), `m_artifactMonitor`, `m_forceDriverRelease`,
`m_pauseResumeGeneration`, `m_lastOpenFailureReason`.

### Constructor / destructor

| Lines | Method | Dependency |
|-------|--------|------------|
| 1150-1181 | `AsioOutputWorker(QObject*)` | touches worker state |
| 1183-1189 | `~AsioOutputWorker()` | touches worker state |

### Output configuration & start

| Lines | Method | Dependency |
|-------|--------|------------|
| 1191-1232 | `configureOutput(int, QByteArray&, QAudioFormat&, qreal, qint64)` | touches worker state |
| 1234-1243 | `prepareOutput(int)` | reads worker state |
| 1245-1248 | `preparedOutputFormat() const` | reads worker state |
| 1250-1285 | `startOutput(int, PcmStreamBuffer*)` | touches worker state |
| 1287-1324 | `finishOutput(int)` | touches worker state |
| 1326-1351 | `pauseOutput(int, int)` | touches worker state |
| 1353-1424 | `resumeOutput(int, int)` | touches worker state |
| 1426-1475 | `forceReleaseDriver()` | touches worker state |
| 1800-1876 | `releaseOutput(int, bool)` | touches worker state |

### Recovery & reset

| Lines | Method | Dependency |
|-------|--------|------------|
| 1477-1483 | `startRecoveryStatus()` | touches worker state |
| 1485-1491 | `recoveryElapsedMs() const` | reads worker state |
| 1493-1499 | `emitRecoveryStatus()` | reads worker state |
| 1501-1506 | `resetRecoveryStatus()` | touches worker state |
| 1508-1664 | `handleCallbackWatchdogRecovery()` | touches worker state |
| 1666-1798 | `handleDriverResetRequest()` | touches worker state |

### Accessors & utilities

| Lines | Method | Dependency |
|-------|--------|------------|
| 1878-1881 | `lastOpenFailureReason() const` | reads worker state |
| 1883-1886 | `notifyResetRequest()` | touches worker state |
| 1888-1892 | `setVolume(qreal)` | touches worker state |

### Render callback (real-time audio path)

| Lines | Method | Dependency |
|-------|--------|------------|
| 1894-1995 | `renderCallback(long)` | touches worker state |

### Signals (lines 1997-2002)

`firstBufferSwitchReceived`, `positionUpdated`, `released`, `stateChanged`,
`statusMessage`

### Private helpers

| Lines | Method | Dependency |
|-------|--------|------------|
| 2005-2010 | `classifyEndpointOpenFailure() const` | reads worker state |
| 2012-2290 | `openDriver()` | touches worker state |
| 2292-2303 | `clearAsioBuffers(long)` | touches worker state |
| 2305-2322 | `sampleTypeBytes(ASIOSampleType) const` | pure helper |
| 2324-2350 | `normalizedSample(const char*) const` | reads player state |
| 2352-2397 | `writeChannel(void*, const char*, int, int, int, ASIOSampleType, qreal) const` | reads player state |
| 2399-2406 | `startArtifactMonitoring()` | touches worker state |
| 2408-2420 | `updateRenderMirrorWindow()` | touches worker state |
| 2422-2456 | `finishArtifactMonitoring()` | touches worker state |

## WindowsAsioAudioPlayer class (lines 2548-3797)

Inherits `AudioPlayerBackend`. Dual-worker model: audio thread (AsioOutputWorker)
+ decoder thread.

### Static methods

| Lines | Method | Dependency |
|-------|--------|------------|
| 2548-2551 | `setHostWindowHandle(quintptr)` | reads global state |
| 2553-2599 | `availableAsioOutputDevices()` | pure helper (cached) |
| 2601-2604 | `hasAvailableAsioOutputDevices()` | reads player state |
| 2606-2616 | `isLikelyCreativeDriverId(const QString&)` | pure helper |
| 2618-2654 | `runDriverInitProbe(...)` | pure helper |

### Constructor / destructor

| Lines | Method | Dependency |
|-------|--------|------------|
| 2656-2722 | `WindowsAsioAudioPlayer(QObject*)` | lifecycle-sensitive |
| 2724-2735 | `~WindowsAsioAudioPlayer()` | lifecycle-sensitive |

### Backend identity & source

| Lines | Method | Dependency |
|-------|--------|------------|
| 2737-2740 | `backendId() const` | reads player state |
| 2742-2745 | `backendName() const` | reads player state |
| 2747-2754 | `decoderName() const` | reads player state |
| 2756-2781 | `setSource(const QString&, int, int, const QString&)` | lifecycle-sensitive |
| 2783-2786 | `source() const` | reads player state |

### Playback controls

| Lines | Method | Dependency |
|-------|--------|------------|
| 2788-2827 | `play()` | lifecycle-sensitive |
| 2829-2855 | `pause()` | lifecycle-sensitive |
| 2857-2900 | `stop()` | lifecycle-sensitive |
| 2902-2920 | `seek(qint64)` | lifecycle-sensitive |
| 2922-2930 | `setVolume(qreal)` | lifecycle-sensitive |
| 2932-2935 | `playbackState() const` | reads player state |

### Device selection & query

| Lines | Method | Dependency |
|-------|--------|------------|
| 2937-2940 | `availableOutputDevices() const` | reads player state |
| 2942-2945 | `availableOutputDeviceInfos() const` | reads player state |
| 2947-2950 | `outputDeviceDescription() const` | reads player state |
| 2952-2955 | `outputFormat() const` | reads player state |
| 2957-2960 | `selectedOutputDevice() const` | reads player state |
| 2962-2965 | `selectedOutputDeviceInfo() const` | reads player state |
| 2967-2970 | `selectedOutputDeviceId() const` | reads player state |
| 2972-2975 | `usesDefaultOutputDevice() const` | reads player state |
| 2977-3010 | `setOutputDeviceId(const QByteArray&)` | lifecycle-sensitive |

### Private helpers — audio levels & format

| Lines | Method | Dependency |
|-------|--------|------------|
| 3012-3020 | `emitAudioLevels(qreal, qreal)` | lifecycle-sensitive |
| 3022-3036 | `channelLayoutForCount(int) const` | pure helper |
| 3038-3056 | `locateFfmpegExecutable() const` | reads player state |
| 3058-3068 | `rawInputFormatForSource() const` | reads player state |
| 3070-3078 | `shouldUseLibavDecoder() const` | reads player state |

### Private helpers — playback finalization & teardown

| Lines | Method | Dependency |
|-------|--------|------------|
| 3080-3098 | `finalizePlayback()` | lifecycle-sensitive |
| 3234-3238 | `clearBufferDevice()` | lifecycle-sensitive |
| 3240-3252 | `finishOutputAfterCompletion()` | lifecycle-sensitive + worker |
| 3254-3268 | `releaseOutputResources()` | lifecycle-sensitive + worker |
| 3671-3691 | `stopDecoderWorker(bool)` | lifecycle-sensitive |
| 3693-3713 | `teardownPipeline()` | lifecycle-sensitive |

### Private helpers — session retry

| Lines | Method | Dependency |
|-------|--------|------------|
| 3270-3275 | `cancelSessionRetry()` | lifecycle-sensitive |
| 3277-3291 | `emitAsioBusyRetryStatus()` | reads player state |

### Private helpers — playback state machine

| Lines | Method | Dependency |
|-------|--------|------------|
| 3293-3300 | `setPlaybackState(PlaybackState)` | lifecycle-sensitive |
| 3302-3322 | `startAudioOutputIfReady()` | lifecycle-sensitive + worker |
| 3324-3330 | `startPipeline(qint64)` | lifecycle-sensitive |
| 3332-3408 | `startPipelineAttempt(qint64, int)` | lifecycle-sensitive |
| 3410-3669 | `continueStartPipeline(qint64, int)` | lifecycle-sensitive + worker |
| 3715-3724 | `emitOutputDeviceSelectionChanged()` | reads player state |
| 3726-3751 | `resolveOutputDevice(bool*) const` | reads player state |
| 3753-3795 | `selectOutputFormat(const AudioOutputDeviceInfo&, QString*) const` | reads player state |

### Slot handlers (Qt signal handlers)

| Lines | Method | Dependency |
|-------|--------|------------|
| 3100-3109 | `handleAudioFirstBufferSwitch(int)` | lifecycle-sensitive |
| 3111-3118 | `handleAudioPositionUpdated(int, qint64)` | lifecycle-sensitive |
| 3120-3166 | `handleAudioStateChanged(int, int, int)` | lifecycle-sensitive |
| 3168-3174 | `handleDecoderDataAvailable(int)` | lifecycle-sensitive |
| 3176-3185 | `handleDecoderError(int, const QString&)` | lifecycle-sensitive |
| 3187-3232 | `handleDecoderFinished(int, int, int, const QString&)` | lifecycle-sensitive |

## Dependency classification summary

| Dependency class | Count | Description |
|-----------------|-------|-------------|
| **Pure helper (no state)** | ~38 | Registry reads, SEH wrappers, format conversions, name lookups, sample math |
| **Reads player state** | ~14 | Const methods, device queries, format selectors |
| **Touches worker state** | ~25 | All AsioOutputWorker mutators, render callback, recovery |
| **Lifecycle-sensitive** | ~30 | All WindowsAsioAudioPlayer methods that mutate playback state, threads, sessions |

## Day 2 ready checklist

### Extraction target: driver discovery helpers

The following pure helpers can be moved to
`src/backends/asio/windowsasioaudioplayer_discovery.cpp` without touching
worker or player lifecycle state:

**Move candidates (pure helpers, no state):**

- `utf16StringFromRegistryValue` (L250)
- `appendAsioRegistryEntries` (L277)
- `registeredAsioDrivers` (L316)
- `parseClsid` (L411)
- `createAsioDriver` (L417)
- `safeAsioInit` (L456)
- `safeAsioRelease` (L471)
- `enumAsioHostWindow` (L341) — reads global `g_asioHostWindowHandle`
- `asioHostWindow` (L353) — reads global `g_asioHostWindowHandle`
- `asioHostWindowCandidates` (L388) — reads global `g_asioHostWindowHandle`

**Also move (structs used only by discovery):**

- `AsioDriverEntry` (L178-181)
- `AsioHostWindowCandidate` (L183-186)
- `AsioHostWindowSearch` (L336-339)

**Leave in place (touches worker state or lifecycle):**

- `AsioOutputWorker::openDriver` (L2012) — touches worker state
- `WindowsAsioAudioPlayer::setHostWindowHandle` (L2548) — static, but reads global
- `WindowsAsioAudioPlayer::availableAsioOutputDevices` (L2553) — static cached
- `WindowsAsioAudioPlayer::hasAvailableAsioOutputDevices` (L2601)
- `WindowsAsioAudioPlayer::runDriverInitProbe` (L2618) — static probe

**Expected new file:** `src/backends/asio/windowsasioaudioplayer_discovery.cpp`

**CMake change:** Add `windowsasioaudioplayer_discovery.cpp` to the ASIO
sources list in `CMakeLists.txt`.

**Validation commands:**

```powershell
.\scripts\build-app-msvc.cmd -BuildDir build-opencode-asio-discovery -Configuration Debug -FfmpegAudioCoreRoot D:\AI\Codex\AudioPlayer-codex-0523-asio\build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc
scripts\run-playback-smoke.ps1 -BuildDir build-opencode-asio-discovery -Configuration Debug -Source D:\AI\Codex\AudioPlayer\build-codex-asio-format-fallback\fixtures\sine-1khz-352800-stereo-s24.wav -AsioOutputIndex 0 -QuitAfterMs 18000 -RequirePlaying
scripts\test-harness-reports.ps1 -LatestSmoke -BuildDir build-opencode-asio-discovery
```

## Day 3 completion: format/session extraction

### Extracted files

- `src/backends/asio/windowsasioaudioplayer_formats.cpp` / `.h` — `AsioFormats` namespace
- `src/backends/asio/windowsasioaudioplayer_sessionprobe.cpp` / `.h` — `AsioSessionProbe` namespace

### AsioFormats functions (pure, no state)

| Function | Original lines |
|----------|---------------|
| `pcmCodecName` | 379-395 |
| `pcmSampleFormatName` | 397-413 |
| `pcmMuxerName` | 415-431 |
| `pcmStreamFormatFromQAudioFormat` | 433-460 |
| `appendUniqueSampleRate` | 462-468 |
| `sourcePreferredSampleRateCandidates` | 470-505 |
| `sampleMagnitude` | 507-533 |

### AsioSessionProbe functions (pure, COM local)

| Function | Original lines |
|----------|---------------|
| `isCreativeAsioDriver` | 560-565 |
| `resolveWasapiEndpointForAsioDriver` | 567-647 |
| `isAudioEndpointBusy` | 649-697 |
| `checkWasapiSessionsForEndpoint` | 710-795 |
| `hasExternalWasapiRenderSessionsForAsioDriver` | 797-821 |
| `hasActiveExternalWasapiRenderSessionsForAsioDriver` | 822-825 |
| `hasAnyExternalWasapiRenderSessionsForAsioDriver` | 827-830 |
| `WasapiSessionCheckResult` (struct) | 699-708 |

### Next safe ASIO split

The next safest split is **read-only worker mapping** — do not move
`AsioOutputWorker` yet. Map the worker methods by dependency class
(touches worker state, reads worker state, lifecycle-sensitive) before
attempting extraction. See the dependency summary in the main map above.

## Day 5 ready checklist: utility/state-name helpers extraction

### Extraction target: utility and state-name pure helpers

The following pure helpers can be moved to
`src/backends/asio/windowsasioaudioplayer_utils.cpp` without touching
worker or player lifecycle state:

**Move candidates (pure helpers, no state):**

- `playbackStateName` (L92) — state name helper
- `audioStateName` (L108) — state name helper
- `toolExecutableOverride` (L124) — environment config
- `hwndText` (L140) — Win32 utility
- `asioDriverError` (L148) — ASIO error string
- `asioResultOk` (L158) — ASIO error check
- `boundedEnvInt` (L367) — environment config

**Move candidates (SEH-wrapped ASIO call wrappers, no state):**

- `safeAsioStart` (L163) — pure helper (SEH)
- `safeAsioStop` (L178) — pure helper (SEH)
- `safeAsioCreateBuffers` (L304) — pure helper (SEH)
- `safeAsioDisposeBuffers` (L325) — pure helper (SEH)
- `safeAsioOutputReady` (L340) — pure helper (SEH)

**Leave in place (touches worker state or lifecycle):**

- `AsioOutputWorker::normalizedSample` (L2324) — reads player state
- `AsioOutputWorker::writeChannel` (L2352) — reads player state
- All other `AsioOutputWorker` methods — touches worker state
- All `WindowsAsioAudioPlayer` methods — lifecycle-sensitive

**Expected new file:** `src/backends/asio/windowsasioaudioplayer_utils.cpp`

**CMake change:** Add `windowsasioaudioplayer_utils.cpp` to the ASIO
sources list in `CMakeLists.txt`.

**Validation commands:**

```powershell
.\scripts\build-app-msvc.cmd -BuildDir build-opencode-asio-utils -Configuration Debug -FfmpegAudioCoreRoot E:\AI\OpenCode\AudioPlayer\build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc
scripts\run-playback-smoke.ps1 -BuildDir build-opencode-asio-utils -Configuration Debug -Source build-opencode-asio-utils\fixtures\sine-1khz-5s.wav -AsioOutputIndex 0 -QuitAfterMs 18000 -RequirePlaying
scripts\test-harness-reports.ps1 -LatestSmoke -BuildDir build-opencode-asio-utils
```

**Stop conditions:**

- Build fails twice after narrowing scope
- Any moved function touches worker or player state
- ASIO lifecycle boundaries become unclear
- Smoke test returns FAIL

## Day 5 completion: utility/state-name helpers extraction

### Extracted files

- `src/backends/asio/windowsasioaudioplayer_utils.cpp` / `.h` — `AsioUtils` namespace

### AsioUtils functions (pure, no state)

| Function | Category |
|----------|----------|
| `playbackStateName` | State name helper |
| `audioStateName` | State name helper |
| `toolExecutableOverride` | Environment config |
| `hwndText` | Win32 utility |
| `asioDriverError` | ASIO error string |
| `asioResultOk` | ASIO error check |
| `boundedEnvInt` | Environment config |
| `safeAsioStart` | SEH wrapper |
| `safeAsioStop` | SEH wrapper |
| `safeAsioCanSampleRate` | SEH wrapper |
| `safeAsioSetSampleRate` | SEH wrapper |
| `safeAsioGetSampleRate` | SEH wrapper |
| `safeAsioGetChannels` | SEH wrapper |
| `safeAsioGetLatencies` | SEH wrapper |
| `safeAsioGetBufferSize` | SEH wrapper |
| `safeAsioGetChannelInfo` | SEH wrapper |
| `safeAsioCreateBuffers` | SEH wrapper |
| `safeAsioDisposeBuffers` | SEH wrapper |
| `safeAsioOutputReady` | SEH wrapper |
| `sampleTypeBytes` | ASIO sample type byte size |
| `AsioSampleType` (enum) | ASIO sample type constants |

### Constants moved to AsioUtils namespace

- `kAsioOk`, `kAsioSuccess`, `kAsioNotPresent`

### Validation

- Build: `scripts\build-app-msvc.cmd -BuildDir build-opencode-asio-utils
  -Configuration Debug -FfmpegAudioCoreRoot
  E:\AI\OpenCode\AudioPlayer\build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc`.
  Build log:
  `build-claude-logs\build-app-msvc-4eb580d064c545f8be283d47b684d46a.log`.
- Validation: Creative ASIO index 0 smoke passed with
  `scripts\run-playback-smoke.ps1 -BuildDir build-opencode-asio-utils
  -Configuration Debug -Source build-opencode-asio-utils\fixtures\sine-1khz-minus18db-48k-stereo.wav
  -AsioOutputIndex 0 -QuitAfterMs 18000 -RequirePlaying`.
  Report: `build-opencode-asio-utils\cache\logs\player-smoke-20260530-092937-688-2c9a115f.harness.json`.
  Observed: `loadedBackend=Windows ASIO`, `asioSelectedDescription=Creative Sound Blaster ASIO Device`,
  `asioConfigureRate=48000`, `asioActiveSampleRate=48000`, `asioFirstBufferSwitchObserved=True`,
  `reportResult=PASS`, `submittedPcmConclusion=ASIO submitted PCM clean`.
- Validation: Realtek ASIO index 1 smoke passed with same build.
  Report: `build-opencode-asio-utils\cache\logs\player-smoke-20260530-093008-543-19ec8cfc.harness.json`.
  Observed: `asioSelectedDescription=Realtek ASIO`, `asioConfigureRate=48000`,
  `asioActiveSampleRate=48000`, `asioFirstBufferSwitchObserved=True`, `reportResult=PASS`.
- Branch: `opencode-0528` (current working branch).

## AsioOutputWorker read-only mapping

### Class scope

`AsioOutputWorker` spans approximately L111-L1575 in the current file (~1465 lines).
Contains 1 enum, 21 public methods, 5 signals, 7 private methods, 25 member variables.

### Dependency classification

#### A. Extracted to AsioUtils (pure, no state)

| Method | Status |
|--------|--------|
| `sampleTypeBytes(AsioSampleType)` | Moved to `AsioUtils::sampleTypeBytes` |

#### B. Could become pure with signature changes

| Method | Current dependency | Required signature change |
|--------|-------------------|--------------------------|
| `normalizedSample(const char*)` | Reads `m_outputFormat.sampleFormat()` | Add `QAudioFormat::SampleFormat` parameter |
| `writeChannel(...)` | Reads `m_bufferSize`, calls `normalizedSample` | Add `long bufferSize` + format parameter |
| `clearAsioBuffers(long)` | Reads `m_bufferInfos`, `m_channelInfos`, `m_bufferSize` | Add 3 parameters |
| `classifyEndpointOpenFailure()` | Reads `m_driverId`, calls `AsioSessionProbe` | Add `QByteArray driverId` parameter |

These are not recommended for extraction in the short term — `writeChannel` and
`normalizedSample` are in the render callback hot path.

#### C. Must stay in class (tight state coupling)

**Lifecycle group** — `configureOutput`, `prepareOutput`, `startOutput`,
`finishOutput`, `pauseOutput`, `resumeOutput`, `forceReleaseDriver`, `releaseOutput`:

All share: `m_driver`, `m_started`, `m_buffersCreated`, `m_comInitialized`,
`m_buffer`, `m_sessionId`, `m_bufferInfos`, `m_channelInfos`, `m_bufferSize`,
`m_outputFormat`, `m_driverId`, `m_volume`, `m_completionPosted`,
`m_callbackCount`, `m_forceDriverRelease`, `m_pauseResumeGeneration`.

**Recovery group** — `handleCallbackWatchdogRecovery`, `handleDriverResetRequest`:

Superset of lifecycle state + `m_callbackWatchdogRecoveryCount`,
`m_callbackWatchdogRecoveryStartMs`, `m_recoveryPendingFirstBufferSwitch`,
`m_resetRequestCount`.

**Recovery helpers** — `startRecoveryStatus`, `recoveryElapsedMs`,
`emitRecoveryStatus`, `resetRecoveryStatus`:

Share `m_callbackWatchdogRecoveryStartMs`, `m_callbackWatchdogRecoveryCount`,
`m_recoveryPendingFirstBufferSwitch`.

**Render callback** — `renderCallback`:

Reads/writes `m_buffer`, `m_bufferInfos`, `m_channelInfos`, `m_outputFormat`,
`m_bufferSize`, `m_volume`, `m_callbackCount`, `m_started`, `m_completionPosted`,
`m_renderedFrames`, `m_artifactMonitor*`, `m_driver`.

**openDriver** (~370 lines) — touches nearly all members.

**Artifact monitoring** — `startArtifactMonitoring`, `updateRenderMirrorWindow`,
`finishArtifactMonitoring`: share `m_artifactMonitor*`, `m_bufferSize`,
`m_outputFormat`.

#### D. Accessors (read-only, stay in class)

| Method | Returns |
|--------|---------|
| `preparedOutputFormat()` | `m_outputFormat` |
| `lastOpenFailureReason()` | `m_lastOpenFailureReason` |
| `lastOpenFailureDetail()` | `m_lastOpenFailureDetail` |
| `notifyResetRequest()` | atomic fetch_add on `m_resetRequestCount` |
| `setVolume(qreal)` | locked write `m_volume` |

#### E. ASIO callback globals (anonymous namespace, must stay global)

| Function | Role |
|----------|------|
| `asioBufferSwitch` | Calls `g_callbackWorker->renderCallback()` |
| `asioSampleRateDidChange` | Log only |
| `asioMessage` | Calls `g_callbackWorker->notifyResetRequest()` |
| `asioBufferSwitchTimeInfo` | Delegates to `asioBufferSwitch` |

### Shared state dependency graph

```
m_driver ─────────┬─ openDriver (create/destroy)
                  ├─ startOutput / finishOutput / pauseOutput / resumeOutput
                  ├─ forceReleaseDriver / releaseOutput
                  ├─ handleCallbackWatchdogRecovery / handleDriverResetRequest
                  └─ renderCallback (outputReady)

m_started ────────┬─ all lifecycle methods
                  ├─ renderCallback
                  └─ positionTimer lambda

m_buffersCreated ─┬─ openDriver / resumeOutput / forceReleaseDriver / releaseOutput
                  └─ recovery methods

m_comInitialized ─┬─ openDriver / forceReleaseDriver / releaseOutput
                  └─ destructor + recovery methods

m_buffer ─────────┬─ startOutput (set)
                  ├─ renderCallback (read)
                  └─ finishOutput / releaseOutput (clear)

m_sessionId ──────┬─ all public methods (guard check)
                  └─ renderCallback / recovery methods

m_callbackCount ──┬─ renderCallback (atomic ++)
                  ├─ startOutput / finishOutput (reset)
                  └─ recovery methods (reset)
```

### Extraction feasibility summary

| Target | Feasibility | Risk | Recommendation |
|--------|-------------|------|----------------|
| `sampleTypeBytes` → `AsioUtils` | ✅ Done | None | Completed |
| `normalizedSample` → change signature | ⚠️ Needs signature change | Render hot path | Low priority |
| `writeChannel` → change signature | ⚠️ Needs signature change | Render hot path | Low priority |
| `clearAsioBuffers` → change signature | ⚠️ Needs signature change | Low | Possible |
| `classifyEndpointOpenFailure` → change signature | ⚠️ Needs signature change | Low | Possible |
| Other methods | ❌ Not recommended | Too much state coupling | Keep in class |
| Whole worker to separate file | ⚠️ Medium-term goal | ~1465 lines + callback globals | Mapping done, no immediate extraction |
