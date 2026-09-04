# WASAPI worker code map

Read-only risk map for
`src/backends/wasapi/windowswasapiaudioplayer_worker.h` (3596 lines). Use this
map to identify safe split boundaries and functions that must stay together
until endpoint or loopback evidence exists. Keep durable workflow rules in
`AGENTS.md` and `docs/dev/*.md`.

This is a risk map, not a refactor plan. Mark the functions that must stay
together until endpoint or loopback evidence exists.

## File structure

```
Lines 1-2:     Include guard
Lines 4-6:     File-level comment
Lines 8-43:    #include statements
Lines 45-503:  Anonymous namespace (free functions, constants, structs)
Lines 505-3594: class WasapiOutputWorker : public QObject
Line 3596:     #endif
```

## Includes

### Project includes (lines 8-11)

`audioplayerbackend.h`, `audioartifactmonitor.h`, `ffmpegpcmshared.h`,
`playerlogger.h`

### Qt includes (lines 13-29)

`QAudioDevice`, `QCryptographicHash`, `QDateTime`, `QDir`, `QFile`,
`QFileInfo`, `QJsonDocument`, `QJsonObject`, `QMediaDevices`, `QProcess`,
`QProcessEnvironment`, `QElapsedTimer`, `QThread`, `QTimer`,
`QVarLengthArray`, `QWinEventNotifier`, `QtEndian`

### Windows/WASAPI includes (lines 35-38)

`<audioclient.h>`, `<ksmedia.h>`, `<mmdeviceapi.h>`, `<objbase.h>`

### C++ standard includes (lines 40-43)

`<cmath>`, `<cstring>`, `<limits>`, `<utility>`

## Anonymous namespace (lines 45-503)

### Constants (lines 47-81)

#### Session/buffer generation constants

| Line | Name | Value | Purpose |
|------|------|-------|---------|
| 47 | `kRecoveryStartupSilenceMs` | 48 | Recovery startup silence |
| 48 | `kHotReconfigureStartupSilenceMs` | 16 | Hot reconfigure silence |
| 49 | `kActiveSwitchRebuildStartupSilenceMs` | 8 | Active switch rebuild silence |
| 50 | `kRecoveryWarmupSilenceMs` | 32 | Recovery warmup silence |
| 51 | `kSeekResumeStartupSilenceMs` | 8 | Seek resume silence |
| 52 | `kSeekResumeWarmupDiscardMs` | 8 | Seek resume warmup discard |
| 55 | `kSeekResumeFfmpegInitialBurstSeconds` | 1.5 | Decoder burst config |
| 57 | `kRecoveryStablePositionAdvanceMs` | 120 | Recovery position advance |
| 61 | `kSeekResumeStartupThresholdMs` | 100 | Seek resume threshold |
| 62 | `kStabilityModeSeekResumeStartupThresholdMs` | 200 | Stability mode threshold |
| 65 | `kActiveSwitchPostInvalidationStartupSilenceMs` | alias | Post-invalidation silence |

#### First-data/fade guard constants

| Line | Name | Value | Purpose |
|------|------|-------|---------|
| 53 | `kDeferredFadeInDelayMs` | 24 | Deferred fade-in delay |
| 54 | `kSeekResumeStreamFadeInDelayMs` | 0 | Seek resume fade delay |
| 58 | `kPcmFadeInDurationMs` | 32 | PCM fade-in duration |
| current | `kPcmFadeOutDurationMs` | 80 | Normal-stop PCM fade-out duration |
| 59 | `kSeekResumePcmFadeInDurationMs` | 24 | Seek resume PCM fade |
| 63 | `kActiveSwitchRebuildPcmFadeInDurationMs` | alias | Active switch fade |
| 64 | `kActiveSwitchFirstBlockMaxFadeGain` | 0.50 | First block max gain |
| 66 | `kActiveSwitchPostInvalidationPcmFadeInDurationMs` | alias | Post-invalidation fade |
| 67 | `kActiveSwitchPostInvalidationFirstBlockMaxFadeGain` | alias | Post-invalidation gain |
| 70 | `kActiveSwitchEntryBridgeStreamGain` | 1.0f | Bridge stream gain |

#### Output configuration constants

| Line | Name | Value | Purpose |
|------|------|-------|---------|
| 56 | `kOutputDeviceChangeDebounceMs` | 180 | Device change debounce |
| 68 | `kActiveSwitchInvalidationTaperGain` | 0.20f | Invalidation taper gain |
| 69 | `kActiveSwitchInvalidationTaperHoldMs` | 24 | Invalidation taper hold |
| 73 | `kDefaultSpatialEndpointFlushMs` | 200 | Spatial endpoint flush |
| 74 | `kDefaultSpatialEndpointSettleMs` | 150 | Spatial endpoint settle |
| 80 | `kExclusiveBufferDuration` | 1000000 | Exclusive buffer (0.1s) |
| 81 | `kStabilityModeOutputBufferMs` | 500 | Stability mode buffer |

#### Diagnostics constants

| Line | Name | Value | Purpose |
|------|------|-------|---------|
| 71 | `kRenderMirrorWindowMs` | 1000 | Render mirror window |
| 72 | `kSubmittedTailWindowMs` | 500 | Submitted tail window |

#### Environment variable names (lines 75-79)

| Line | Name | Value |
|------|------|-------|
| 75 | `kWasapiLibavDecoderEnv` | `"AUDIOPLAYER_WASAPI_LIBAV_DECODER"` |
| 76 | `kLegacyWasapiLibavSeekResumeEnv` | `"AUDIOPLAYER_WASAPI_LIBAV_SEEK_RESUME"` |
| 77 | `kWasapiRenderMirrorWindowMsEnv` | `"AUDIOPLAYER_WASAPI_RENDER_MIRROR_WINDOW_MS"` |
| 78 | `kWasapiExclusiveEnv` | `"AUDIOPLAYER_WASAPI_EXCLUSIVE"` |
| 79 | `kWasapiCreativeChannelReorderEnv` | `"AUDIOPLAYER_WASAPI_CREATIVE_CHANNEL_REORDER"` |

### Structs (lines 259-320)

| Lines | Struct | Category | Risk |
|-------|--------|----------|------|
| 259-268 | `ActiveSwitchBoundaryPolicy` | Session/buffer generation | Must stay with `configureOutput` and `activeSwitchBoundaryPolicyForOutputFormats` |
| 270-295 | `WasapiArtifactTrackingConfig` | Diagnostics | Must stay with all artifact tracking / render mirror functions |
| 297-309 | `RenderedBlockMetrics` | Diagnostics | Must stay with `renderedBlockMetricsForChunk`, `analyzeArtifactBlock`, boundary envelope functions |
| 311-320 | `PcmFadeApplication` | First-data/fade guards | Must stay with `applyPcmFadeIn` |

### Free functions by category (lines 83-501)

#### Output configuration

| Lines | Function | Risk |
|-------|----------|------|
| 83-89 | `envFlagDisabled(const QString&)` | Standalone utility |
| 91-96 | `isCreativeG5WasapiDeviceDescription(const QString&)` | Must stay with `creativeWasapiChannelOrder*` |
| 98-102 | `creativeWasapiChannelOrderWorkaroundEnabled()` | Must stay with Creative channel reorder group |
| 104-123 | `creativeWasapiChannelOrderFilter(...)` | Must stay with Creative channel reorder group |
| 125-137 | `channelMaskFromWaveFormatData(const QByteArray&)` | Must stay with `channelLayoutForMask` |
| 139-153 | `channelLayoutForCountFallback(int)` | Must stay with `channelLayoutForMask` |
| 155-198 | `channelLayoutForMask(DWORD, int)` | Must stay with `channelMaskFromWaveFormatData` |
| 200-203 | `channelLayoutForWaveFormatData(...)` | Must stay with channel layout group |
| 213-233 | `boundedEnvInt(...)` | Standalone utility |
| 235-247 | `wasapiLibavDecoderDisabled()` | Must stay with env constants |
| 249-257 | `bufferDurationForBytes(...)` | Standalone utility |
| 387-395 | `endpointIdFromQtId(const QByteArray&)` | Must stay with `configureOutput` |
| 397-414 | `channelMaskForCount(int)` | Must stay with `buildWaveFormat` |
| 445-485 | `buildWaveFormat(...)` | Must stay with `configureOutput` |

#### Session/buffer generation

| Lines | Function | Risk |
|-------|----------|------|
| 353-369 | `activeSwitchBoundaryPolicyForOutputFormats(...)` | Must stay with `ActiveSwitchBoundaryPolicy` struct |

#### Render loop

| Lines | Function | Risk |
|-------|----------|------|
| 416-426 | `readInt24Sample(const char*)` | Must stay with `writeInt24Sample`, `copyConvertedFramesToRenderBuffer` |
| 428-434 | `writeInt24Sample(qint32, char*)` | Must stay with `readInt24Sample` |
| 436-443 | `hasSamePcmLayout(...)` | Must stay with `copyConvertedFramesToRenderBuffer` |

#### Diagnostics

| Lines | Function | Risk |
|-------|----------|------|
| 205-211 | `channelMaskText(DWORD)` | Standalone utility |
| 322-340 | `pcmEncodingName(PcmSampleEncoding)` | Standalone utility |
| 371-385 | `playbackStateName(AudioPlayerBackend::PlaybackState)` | Standalone utility |
| 487-501 | `mapWasapiError(HRESULT, bool)` | Must stay with `handleFatalError` |

#### Resource release

| Lines | Function | Risk |
|-------|----------|------|
| 342-351 | `safeRelease<T>(T*&)` | Used by all WASAPI release paths |

## WasapiOutputWorker class (lines 505-3594)

Single class implementing the entire WASAPI audio render pipeline.

### Public methods

#### Constructor / destructor

| Lines | Method | Category | Risk |
|-------|--------|----------|------|
| 510-537 | `WasapiOutputWorker(QObject*)` | Resource release / init | Standalone initialization |
| 539-545 | `~WasapiOutputWorker()` | Resource release | Must stay with `releaseOutput` |

#### Output configuration (lines 547-1155, 1275-1344, 1511-1534)

| Lines | Method | Category | Risk |
|-------|--------|----------|------|
| 547-900 | `configureOutput(...)` | Output configuration | Must stay with `buildWaveFormat`, `endpointIdFromQtId`, `ActiveSwitchBoundaryPolicy`, `WasapiArtifactTrackingConfig` |
| 902-1155 | `flushSpatialEndpoint(...)` | Output configuration | Must stay with `configureOutput` (spatial audio flush for endpoint settling) |
| 1275-1286 | `prepareForOutputDeviceChange(int)` | Output configuration | Must stay with `fadeOutStreamGainBeforeStop` |
| 1288-1305 | `prepareForActiveOutputInvalidation(int)` | Output configuration | Must stay with invalidation taper group |
| 1307-1318 | `restoreAfterCancelledOutputDeviceChange(int)` | Output configuration | Must stay with `prepareForOutputDeviceChange` |
| 1320-1331 | `restoreAfterCancelledActiveOutputInvalidation(int)` | Output configuration | Must stay with `prepareForActiveOutputInvalidation` |
| 1333-1344 | `restoreActiveOutputInvalidationTaper(int)` | Output configuration | Must stay with `prepareForActiveOutputInvalidation` |
| 1511-1534 | `setVolume(qreal)` | Output configuration | Must stay with `applyOutputVolume`, `resetVolumeRamp` |
| 3581 | `exclusiveModeActive() const` (inline) | Output configuration | Standalone accessor |

#### Render loop (lines 1157-1273)

| Lines | Method | Category | Risk |
|-------|--------|----------|------|
| 1157-1217 | `startOutput(int, PcmStreamBuffer*)` | Render loop | Must stay with `renderAvailableFrames`, `configureOutput` |
| 1219-1245 | `pauseOutput(int)` | Render loop | Must stay with `startOutput` |
| 1247-1273 | `resumeOutput(int)` | Render loop | Must stay with `startOutput`, `pauseOutput` |

#### Resource release (lines 1346-1509)

| Lines | Method | Category | Risk |
|-------|--------|----------|------|
| 1346-1509 | `releaseOutput(int, bool)` | Resource release | Must stay with destructor, `drainMutedPaddingBeforeReset`, COM lifecycle |

#### Signals (lines 1536-1539)

| Line | Signal | Category |
|------|--------|----------|
| 1537 | `positionUpdated(int, qint64)` | Render loop |
| 1538 | `released(int)` | Resource release |
| 1539 | `stateChanged(int, int, int)` | Render loop |

### Private methods

#### Output configuration

| Lines | Method | Risk |
|-------|--------|------|
| 1542-1556 | `ensureComInitialized()` | Must stay with `configureOutput`, `releaseOutput`, COM lifecycle |
| 1960-1975 | `resetVolumeRamp(...)` | Must stay with `setVolume`, `applyOutputVolume` |

#### First-data/fade guards

| Lines | Method | Risk |
|-------|--------|------|
| 1558-1570 | `setStreamGain(float)` | Must stay with `fadeStreamGainTo`, `fadeOutStreamGainBeforeStop` |
| 1572-1606 | `fadeStreamGainTo(float, bool)` | Must stay with `setStreamGain`, fade group |
| 1608-1634 | `fadeOutStreamGainBeforeStop()` | Must stay with `releaseOutput`, `prepareForOutputDeviceChange` |
| 1723-1741 | `fadeInStreamGainIfNeeded()` | Must stay with `scheduleFadeInStreamGain` |
| 1743-1764 | `scheduleFadeInStreamGain()` | Must stay with `fadeInStreamGainIfNeeded`, `renderAvailableFrames` |
| 1940-1958 | `resetPcmFadeIn(...)` | Must stay with `applyPcmFadeIn`, `configureOutput` |
| 1977-2024 | `applyGainToSample(char*, qreal)` | Must stay with `applyPcmFadeIn`, `applyOutputVolume` |
| 2127-2136 | `currentFadeEndpointGain() const` | Must stay with `applyPcmFadeIn` |
| 2138-2200 | `applyPcmFadeIn(QByteArray&)` | Must stay with `PcmFadeApplication` struct, `resetPcmFadeIn` |
| 2202-2209 | `shouldGuardActiveSwitchFirstDataBlock() const` | Must stay with `guardActiveSwitchFirstDataBlockFade` |
| 2211-2242 | `guardActiveSwitchFirstDataBlockFade(const QByteArray&)` | Must stay with `shouldGuardActiveSwitchFirstDataBlock` |

#### Render loop

| Lines | Method | Risk |
|-------|--------|------|
| 2244-2286 | `applyOutputVolume(QByteArray&)` | Must stay with `setVolume`, `resetVolumeRamp` |
| 2288-2330 | `copyConvertedFramesToRenderBuffer(...)` | Must stay with `readInt24Sample`, `writeInt24Sample`, `hasSamePcmLayout` |
| 2332-2349 | `processedPositionMs()` | Must stay with `processedPositionMsFromPadding` |
| 2351-2361 | `processedPositionMsFromPadding(UINT32) const` | Must stay with `processedPositionMs` |
| 3014-3033 | `emitIdleIfDrained()` | Must stay with `renderAvailableFrames` |
| 3035-3508 | `renderAvailableFrames()` | **CRITICAL**: Core render loop. Must stay with `configureOutput`, `startOutput`, `applyPcmFadeIn`, `applyOutputVolume`, `copyConvertedFramesToRenderBuffer`, `mirrorSubmittedBlock`, `analyzeArtifactBlock`, `guardActiveSwitchFirstDataBlockFade`, `scheduleFadeInStreamGain`, `noteFirstSubmittedPcmAfterSeek`, `emitIdleIfDrained` |

#### Resource release

| Lines | Method | Risk |
|-------|--------|------|
| 1636-1721 | `drainMutedPaddingBeforeReset(UINT32)` | Must stay with `releaseOutput` |
| 1885-1938 | `handleFatalError(HRESULT, bool)` | Must stay with `mapWasapiError`, `releaseOutput` |

#### Diagnostics

| Lines | Method | Risk |
|-------|--------|------|
| 1766-1839 | `noteFirstSubmittedPcmAfterSeek(...)` | Must stay with seek resume timing group |
| 1841-1883 | `logSeekResumeLatencyIfNeeded(qint64)` | Must stay with seek resume timing group |
| 2026-2053 | `readNormalizedSample(const char*, ...) const` | Must stay with `renderedBlockMetricsForChunk` |
| 2055-2058 | `metricText(double) const` | Standalone utility |
| 2060-2063 | `fineMetricText(double) const` | Standalone utility |
| 2065-2068 | `renderedBlockMetricsForChunk(const QByteArray&) const` | Must stay with two-arg overload |
| 2070-2125 | `renderedBlockMetricsForChunk(const QByteArray&, const PcmStreamFormat&) const` | Must stay with `RenderedBlockMetrics` struct, `analyzeArtifactBlock` |
| 2363-2366 | `artifactTrackingEnabled() const` | Standalone utility |
| 2368-2384 | `artifactPlaybackContext(UINT32) const` | Must stay with `artifactRenderContext` |
| 2386-2402 | `artifactRenderContext(...) const` | Must stay with `artifactPlaybackContext` |
| 2404-2414 | `formatJson(const PcmStreamFormat&) const` | Must stay with render mirror group |
| 2416-2421 | `renderMirrorBasePath(int) const` | Must stay with render mirror group |
| 2423-2428 | `previousRunTailFingerprintPath() const` | Must stay with `saveSubmittedTailFingerprint` |
| 2430-2473 | `saveSubmittedTailFingerprint(const QString&)` | Must stay with `appendSubmittedPcmTail`, render mirror group |
| 2475-2585 | `startRenderMirrorCapture(...)` | Must stay with `finishRenderMirrorCapture` |
| 2587-2659 | `finishRenderMirrorCapture(const QString&)` | Must stay with `startRenderMirrorCapture` |
| 2661-2674 | `appendSubmittedPcmTail(...)` | Must stay with `mirrorSubmittedBlock` |
| 2676-2775 | `captureSeekResumeFirst50msSubmittedPcm(...)` | Must stay with seek resume timing group |
| 2777-2827 | `mirrorSubmittedBlock(...)` | Must stay with `appendSubmittedPcmTail`, `captureSeekResumeFirst50msSubmittedPcm` |
| 2829-2849 | `observeArtifactSilence(...)` | Must stay with `artifactTrackingEnabled`, `m_artifactMonitor` |
| 2851-3012 | `analyzeArtifactBlock(...)` | Must stay with `renderedBlockMetricsForChunk`, `artifactPlaybackContext`, `artifactRenderContext`, boundary envelope logging |

### Member variables (lines 3510-3593)

#### Render loop members

`m_buffer` (PcmStreamBuffer\*), `m_renderClient` (IAudioRenderClient\*),
`m_refillEvent` (HANDLE), `m_eventNotifier` (QWinEventNotifier\*),
`m_positionTimer` (QTimer\*), `m_submittedFrames` (quint64),
`m_waitForDataStreak`, `m_lastRenderCallbackTime`

#### Output configuration members

`m_device` (IMMDevice\*), `m_audioClient` (IAudioClient\*),
`m_waveFormatData` (QByteArray), `m_started`, `m_comInitialized`,
`m_shouldUninitializeCom`, `m_targetVolume`, `m_currentVolume`,
`m_volumeRampStartVolume`, `m_volumeRampActive`, `m_volumeRampTotalFrames`,
`m_volumeRampFramesProcessed`, `m_deviceFormat`, `m_bufferFormat`
(PcmStreamFormat), `m_exclusiveModeActive`

#### First-data/fade guard members

`m_streamVolume` (IAudioStreamVolume\*), `m_fadeTimer` (QTimer\*),
`m_fadeStep`, `m_fadeTotalSteps`, `m_fadeStartGain`, `m_fadeTargetGain`,
`m_idleSignaled`, `m_streamFadeInPending`, `m_streamFadeInScheduled`,
`m_streamGain`, `m_stopFadeInProgress`, `m_renderCallbacksDuringStopFade`,
`m_firstDataBlockAfterConfigure`, `m_lastRenderedBlock`,
`m_previousRenderedBlock`, `m_activeSwitchEntryBridgeBlock`,
`m_activeSwitchEntryBridgeFallback`, `m_activeSwitchBoundaryPolicyName`,
`m_activeSwitchFirstBlockMaxFadeGain`, `m_activeSwitchEntryBridgeStreamGain`,
`m_pcmFadeInDurationMs`, `m_pcmFadeTotalFrames`, `m_pcmFadeFramesProcessed`,
`m_streamFadeInDelayMs`

#### Session/buffer generation members

`m_bufferFrameCount`, `m_pendingStartupSilenceFrames`,
`m_configuredStartupSilenceFrames`, `m_configuredStartupSilenceMs`,
`m_pendingRecoveryWarmupFrames`, `m_configuredWarmupDiscardMs`,
`m_sessionId`, `m_outputBufferGeneration`

#### Diagnostics members

`m_artifactMonitor` (AudioArtifactMonitor),
`m_artifactTracking` (WasapiArtifactTrackingConfig),
`m_seekResumeFirstDecodedPcmAfterSeekMs`,
`m_seekResumeFirstSubmittedPcmAfterSeekMs`, `m_seekResumeLatencyLogged`,
`m_seekResumeFirst50msSubmittedPcm`, `m_seekResumeFirst50msTargetFrames`,
`m_seekResumeFirst50msCapturedFrames`,
`m_seekResumeFirst50msStartupSilenceFrames`,
`m_seekResumeFirst50msWarmupFrames`, `m_seekResumeFirst50msRealPcmFrames`,
`m_seekResumeFirst50msLogged`, `m_rateWindowStartTimeMs`,
`m_rateWindowStartFrames`, `m_rateLastLogTimeMs`, `m_terminalErrorLatched`

#### Render mirror members

`m_submittedRenderTail`, `m_submittedRenderTailFormat`, `m_renderMirrorFile`,
`m_renderMirrorMetadata`, `m_renderMirrorMonitor`, `m_renderMirrorActive`,
`m_renderMirrorCapturedFrames`, `m_renderMirrorMaxFrames`,
`m_renderMirrorRawPath`, `m_renderMirrorMetadataPath`

## Risk clusters

These groups of functions must stay together until endpoint or loopback
evidence exists. Do not split them.

### Render loop cluster

The core audio path. Splitting any piece risks breaking the event-driven
render cycle.

- `renderAvailableFrames` (L3035) — **CRITICAL**: core render callback
- `emitIdleIfDrained` (L3014)
- `startOutput` (L1157)
- `pauseOutput` (L1219)
- `resumeOutput` (L1247)
- `copyConvertedFramesToRenderBuffer` (L2288)
- `applyGainToSample` (L1977)
- `applyOutputVolume` (L2244)
- `processedPositionMs` (L2332)
- `processedPositionMsFromPadding` (L2351)
- `readInt24Sample` (L416)
- `writeInt24Sample` (L428)
- `hasSamePcmLayout` (L436)

### Fade system cluster

Multi-layered pop/click mitigation. Splitting risks audible artifacts.

- `setStreamGain` (L1558)
- `fadeStreamGainTo` (L1572)
- `fadeOutStreamGainBeforeStop` (L1608)
- `fadeInStreamGainIfNeeded` (L1723)
- `scheduleFadeInStreamGain` (L1743)
- `resetPcmFadeIn` (L1940)
- `applyPcmFadeIn` (L2138)
- `currentFadeEndpointGain` (L2127)
- `shouldGuardActiveSwitchFirstDataBlock` (L2202)
- `guardActiveSwitchFirstDataBlockFade` (L2211)
- `PcmFadeApplication` struct (L311)
- All fade constants (L53, L54, L58, L59, L63, L64, L66, L67, L70)

### Release pipeline cluster

Teardown order is an invariant: release audio output resources -> stop decoder
-> clear buffer/device.

- `releaseOutput` (L1346)
- `~WasapiOutputWorker` (L539)
- `drainMutedPaddingBeforeReset` (L1636)
- `handleFatalError` (L1885)
- `safeRelease` (L342)
- `fadeOutStreamGainBeforeStop` (L1608)

### configureOutput pipeline cluster

Device enumeration, WASAPI client initialization, format negotiation, and
endpoint setup must stay together.

- `configureOutput` (L547)
- `flushSpatialEndpoint` (L902)
- `ensureComInitialized` (L1542)
- `buildWaveFormat` (L445)
- `endpointIdFromQtId` (L387)
- `channelMaskForCount` (L397)
- `resetPcmFadeIn` (L1940)
- `resetVolumeRamp` (L1960)

### Active-switch invalidation taper cluster

Gain reduction during active output invalidation. Splitting risks pops during
device switching.

- `prepareForActiveOutputInvalidation` (L1288)
- `restoreAfterCancelledActiveOutputInvalidation` (L1320)
- `restoreActiveOutputInvalidationTaper` (L1333)
- `kActiveSwitchInvalidationTaperGain` (L68)
- `kActiveSwitchInvalidationTaperHoldMs` (L69)

### Device change flow cluster

- `prepareForOutputDeviceChange` (L1275)
- `restoreAfterCancelledOutputDeviceChange` (L1307)

### Render mirror system cluster

Raw PCM dump and metadata for post-hoc analysis. Splitting risks losing
diagnostic context.

- `startRenderMirrorCapture` (L2475)
- `finishRenderMirrorCapture` (L2587)
- `mirrorSubmittedBlock` (L2777)
- `appendSubmittedPcmTail` (L2661)
- `captureSeekResumeFirst50msSubmittedPcm` (L2676)
- `saveSubmittedTailFingerprint` (L2430)
- `renderMirrorBasePath` (L2416)
- `previousRunTailFingerprintPath` (L2423)
- `formatJson` (L2404)

### Artifact analysis cluster

Pop/candidate detection and boundary envelope analysis. Splitting risks
breaking the diagnostic evidence chain.

- `analyzeArtifactBlock` (L2851)
- `observeArtifactSilence` (L2829)
- `artifactTrackingEnabled` (L2363)
- `artifactPlaybackContext` (L2368)
- `artifactRenderContext` (L2386)
- `renderedBlockMetricsForChunk` (L2065, L2070)
- `readNormalizedSample` (L2026)
- `RenderedBlockMetrics` struct (L297)
- `WasapiArtifactTrackingConfig` struct (L270)
- `m_artifactMonitor` (L3547)
- Boundary envelope analysis (inside `analyzeArtifactBlock`, L2918-2996)

### Seek resume timing cluster

- `noteFirstSubmittedPcmAfterSeek` (L1766)
- `logSeekResumeLatencyIfNeeded` (L1841)
- All `m_seekResume*` members

## Architecture summary

The file implements a single class `WasapiOutputWorker` (~3090 lines) that
encapsulates the entire WASAPI audio render pipeline:

1. **Configuration layer**: `configureOutput` creates the WASAPI audio client,
   initializes the endpoint (shared or exclusive), sets up the event-driven
   render loop, and configures all fade/silence/warmup parameters.
   `flushSpatialEndpoint` handles spatial audio endpoint settling.

2. **Render loop**: Driven by `QWinEventNotifier` on the WASAPI refill event.
   `renderAvailableFrames` is the core callback that reads decoded PCM from
   the `PcmStreamBuffer`, applies PCM fade-in and output volume, converts
   formats (Int32->Int24), and submits to the WASAPI render client. Startup
   silence injection and warmup discard happen before real PCM is submitted.

3. **Fade guard layer**: A multi-layered fade system prevents audible pops:
   stream-level gain fading via `IAudioStreamVolume`, sample-level PCM fade-in
   via `applyPcmFadeIn`, sample-level PCM fade-out before normal explicit stop,
   active-switch first-data-block gain capping, and invalidation taper gain
   reduction. Output-switch and recovery teardown do not consume the normal-stop
   PCM tail.

4. **Diagnostics layer**: Extensive artifact tracking via
   `AudioArtifactMonitor`, render mirror capture (raw PCM dump + JSON
   metadata), seek-resume latency measurement, submitted-tail fingerprinting,
   boundary envelope analysis for active-switch pop detection, and render rate
   monitoring.

5. **Resource release**: Explicit teardown order in `releaseOutput` — disable
   event notifier, disconnect buffer, drain muted padding, stop audio client,
   reset, release COM objects, close handles, reset all state.
