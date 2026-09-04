# Phase 6 Audit: Platform Abstraction Layer Design

> Generated: 2026-06-13
> Purpose: Abstract platform differences for clean cross-platform architecture

## Design Principles

1. **Backend-specific code stays in backend directories** — WASAPI in `backends/wasapi/`, ALSA in `backends/alsa/`, CoreAudio in `backends/coreaudio/`
2. **Shared abstractions live in `backends/shared/`** — common interfaces, format types, utility functions
3. **Factory pattern already exists** — `AudioPlayerFactory` routes by platform; extend for CoreAudio
4. **Don't over-abstract** — only abstract what genuinely differs across platforms

---

## 1. Audio Output API Abstraction

### Current State
Each backend directly implements `AudioPlayerBackend` (defined in `audioplayerbackend.h`). This is already the correct pattern — no additional abstraction layer needed for the output API itself.

### What's Actually Shared
The following code is duplicated across WASAPI and ALSA backends and should be extracted to `backends/shared/`:

#### 1.1 PCM Format Conversion

**Current**: Each backend has its own sample conversion code.

**Shared interface**:
```cpp
// backends/shared/pcmconverter.h
class PcmConverter {
public:
    static void convertBuffer(const QByteArray &source,
                              QByteArray &target,
                              const PcmStreamFormat &sourceFormat,
                              const PcmStreamFormat &targetFormat);

    static void applyGain(QByteArray &data,
                          const PcmStreamFormat &format,
                          qreal gain);

    static void applyFadeIn(QByteArray &data,
                            const PcmStreamFormat &format,
                            qsizetype frameOffset,
                            qsizetype totalFrames);

    static void applyFadeOut(QByteArray &data,
                             const PcmStreamFormat &format,
                             qsizetype framesProcessed,
                             qsizetype totalFrames);
};
```

**Rationale**: WASAPI worker has `applyGainToSample()`, `applyFadeVolumeAndConvert()`, `copyConvertedFramesToRenderBuffer()`. ALSA worker has `convertFormat()` and inline volume application. Both do the same work.

#### 1.2 Audio Level Calculation

**Current**: ALSA has `AlsaOutputWorker::emitAudioLevels()` with `sampleMagnitude()`. WASAPI has `AudioArtifactMonitor` + inline metric calculation.

**Shared interface**:
```cpp
// backends/shared/audiolevelcalculator.h
class AudioLevelCalculator {
public:
    struct Levels {
        qreal left = 0.0;
        qreal right = 0.0;
        double peak = 0.0;
        double rms = 0.0;
    };

    static Levels calculate(const QByteArray &data,
                            const PcmStreamFormat &format,
                            int channelCount);
};
```

#### 1.3 Position Tracking

**Current**: Both backends calculate `processedPositionMs` from frame counts.

**Shared interface**:
```cpp
// backends/shared/positiontracker.h
class PositionTracker {
public:
    void reset();
    void addProcessedFrames(qint64 frames, int sampleRate);
    qint64 currentPositionMs() const;
    void setPositionMs(qint64 ms);

private:
    qint64 m_processedFrames = 0;
    int m_sampleRate = 0;
    qint64 m_offsetMs = 0;
};
```

---

## 2. Device Enumeration Abstraction

### Current State
- **WASAPI**: Uses `QMediaDevices::audioOutputs()` for listing, `IMMDeviceEnumerator` for endpoint resolution
- **ALSA**: Uses `QMediaDevices::audioOutputs()` only, falls back to hardcoded `"hw:0"` / `"default"`
- **ASIO**: Custom Windows registry enumeration via `WindowsAsioAudioPlayer::availableAsioOutputDevices()`

### Design

```cpp
// backends/shared/audiodeviceenumerator.h
struct AudioDeviceCapabilities {
    QList<int> supportedSampleRates;
    QList<int> supportedChannelCounts;
    QList<PcmSampleEncoding> supportedEncodings;
    bool supportsExclusiveMode = false;
};

class AudioDeviceEnumerator {
public:
    virtual ~AudioDeviceEnumerator() = default;

    virtual QList<QAudioDevice> availableOutputDevices() = 0;
    virtual QString deviceDescription(const QAudioDevice &device) = 0;
    virtual AudioDeviceCapabilities probeCapabilities(const QAudioDevice &device) = 0;
    virtual bool isDefaultDevice(const QAudioDevice &device) = 0;
};
```

### Platform Implementations

| Platform | Implementation Strategy |
|----------|----------------------|
| **Windows (WASAPI)** | `QMediaDevices` for listing + `IAudioClient::IsFormatSupported` for capability probing |
| **Linux (ALSA)** | `snd_device_name_hint()` for listing + `snd_pcm_hw_params_test_*` for capability probing |
| **macOS (CoreAudio)** | `AudioObjectGetPropertyData` with `kAudioHardwarePropertyDevices` + `kAudioStreamPropertyPhysicalFormats` |

**Current gap**: ALSA uses Qt's `QMediaDevices` which doesn't expose ALSA-specific device names. Need to map Qt device IDs to ALSA device strings (`hw:X`, `plughw:X`, `default`).

### Recommended ALSA Device Mapping

```cpp
// backends/alsa/alsadeviceenumerator.cpp
class AlsaDeviceEnumerator : public AudioDeviceEnumerator {
    // Map Qt device ID to ALSA device name
    QString alsaDeviceNameForQtDevice(const QAudioDevice &device) {
        // Qt ALSA backend stores "default" or "hw:X,Y" in device.id()
        return QString::fromUtf8(device.id());
    }

    // Enumerate via snd_device_name_hint()
    QList<QAudioDevice> availableOutputDevices() override {
        void **hints;
        snd_device_name_hint(-1, "pcm", &hints);
        // Filter to output-only devices
        // ...
    }
};
```

---

## 3. Format Negotiation Abstraction

### Current State
- **WASAPI**: Complex negotiation via `WAVEFORMATEXTENSIBLE` + `IAudioClient::IsFormatSupported`
- **ALSA**: `AlsaFormatNegotiator` with `snd_pcm_hw_params_test_*`
- **FFmpeg**: Uses Qt's `QAudioSink` format negotiation

### Design

```cpp
// backends/shared/formatnegotiator.h
struct NegotiatedFormat {
    bool success = false;
    QAudioFormat qtFormat;
    PcmStreamFormat pcmFormat;
    bool exclusiveMode = false;
    QString deviceName;
};

class FormatNegotiator {
public:
    virtual ~FormatNegotiator() = default;

    virtual NegotiatedFormat negotiate(int sourceSampleRate,
                                       int sourceBitDepth,
                                       int sourceChannelCount,
                                       bool exclusiveMode,
                                       bool exactMode) = 0;
};
```

**Note**: Each backend already has its own negotiation logic. This interface mainly standardizes the output contract. The implementation stays backend-specific because the negotiation APIs are fundamentally different (WAVEFORMATEXTENSIBLE vs snd_pcm_hw_params vs AudioStreamBasicDescription).

---

## 4. Exclusive Mode Abstraction

### Platform Comparison

| Aspect | WASAPI | ALSA | CoreAudio |
|--------|--------|------|-----------|
| **API** | `AUDCLNT_SHAREMODE_EXCLUSIVE` | `hw:` device (natural exclusive) | `kAudioDevicePropertyHogMode` |
| **Buffer** | `REFERENCE_TIME` based | `snd_pcm_hw_params_set_buffer_size` | `kAudioDevicePropertyBufferFrameSize` |
| **Fallback** | Auto-fallback to shared | `hw:` → `default` | Release hog mode |
| **Detection** | `exclusiveModeActive()` flag | Implicit (hw: open = exclusive) | `AudioObjectIsPropertySettable` |

### Design

```cpp
// backends/shared/exclusivemodepolicy.h
struct ExclusiveModeConfig {
    bool enabled = false;
    int bufferDurationMs = 100;  // Platform-specific default
    bool allowFallback = true;   // Fall back to shared if exclusive fails
};

class ExclusiveModePolicy {
public:
    virtual ~ExclusiveModePolicy() = default;

    // Attempt to open device in exclusive mode
    // Returns actual mode achieved (may fall back to shared)
    virtual bool apply(void *deviceHandle,
                       const ExclusiveModeConfig &config,
                       const PcmStreamFormat &format) = 0;

    // Release exclusive mode
    virtual void release(void *deviceHandle) = 0;

    // Check if currently in exclusive mode
    virtual bool isActive() const = 0;
};
```

**Note**: This is a thin abstraction. Each platform's implementation is fundamentally different. The main value is standardizing the fallback behavior and configuration.

---

## 5. COM Initialization (Windows-Only)

### Current State
- `ScopedComInitializer` in `windowswasapiaudioplayer_output.cpp:57-79`
- `ensureComInitialized()` in worker

### Design
No abstraction needed — this is Windows-specific. The existing `#if defined(Q_OS_WINDOWS)` guards are sufficient.

**Recommendation**: Move `ScopedComInitializer` to `backends/shared/cominitializer.h` guarded by `#ifdef Q_OS_WINDOWS` so it can be reused if needed.

---

## 6. Registry Access (Windows-Only)

### Current State
- ASIO backend reads Windows registry for driver enumeration
- No other backend needs registry access

### Design
No abstraction needed. ASIO is Windows-only. The registry access code stays in `backends/asio/`.

---

## 7. Driver Management (ASIO — Windows-Only)

### Current State
- ASIO SDK interface in `asio_interface.h`
- Driver discovery via registry
- Session probe for multi-device detection

### Design
No abstraction needed. ASIO is Windows-only and has no cross-platform equivalent.

---

## 8. Output Worker Abstraction

### Current State
- **WASAPI**: `WasapiOutputWorker` — event-driven with `IAudioRenderClient`, `QWinEventNotifier`
- **ALSA**: `AlsaOutputWorker` — polling loop with `snd_pcm_writei`

### Design

```cpp
// backends/shared/audiooutputworker.h
class AudioOutputWorker : public QObject {
    Q_OBJECT
public:
    virtual ~AudioOutputWorker() = default;

    virtual void configure(/* platform-specific params */) = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void setPaused(bool paused) = 0;
    virtual void setVolume(qreal volume) = 0;

signals:
    void audioLevelsChanged(int sessionId, qreal leftLevel, qreal rightLevel);
    void positionUpdated(int sessionId, qint64 positionMs);
    void stateChanged(int sessionId, int state, int error);
    void errorOccurred(int sessionId, const QString &message);
    void finished();
};
```

**Status**: Both workers already emit compatible signals. The main difference is internal:
- WASAPI: Event-driven (`HANDLE m_refillEvent` + `QWinEventNotifier`)
- ALSA: Polling (`snd_pcm_avail_update` + `snd_pcm_wait` + `QThread::msleep`)

The `configure()` method parameters differ too much to unify. Keep backend-specific configure signatures.

---

## 9. Output Recovery Abstraction

### Current State
- **WASAPI**: 3-attempt recovery with increasing delays (250/500/900ms), sophisticated active-switch transaction system
- **ALSA**: 3-attempt recovery with linear backoff (250/500/750ms), simple pipeline restart

### Design

```cpp
// backends/shared/outputrecoverymanager.h
struct RecoveryConfig {
    int maxAttempts = 3;
    QList<int> delayMs = {250, 500, 900};
};

class OutputRecoveryManager {
public:
    void reset();
    bool shouldAttemptRecovery(QtAudio::Error error) const;
    int nextAttemptDelayMs() const;
    void recordAttempt();
    bool isExhausted() const;

private:
    int m_attempt = 0;
    RecoveryConfig m_config;
};
```

**Note**: The recovery *policy* can be shared, but the recovery *action* (rebuild pipeline, reconfigure output, etc.) is backend-specific.

---

## 10. Summary: What to Abstract vs. What to Keep Backend-Specific

### Abstract to `backends/shared/`
| Component | Priority | Effort |
|-----------|----------|--------|
| PCM format conversion (`PcmConverter`) | P1 | ~200 lines |
| Audio level calculation (`AudioLevelCalculator`) | P2 | ~80 lines |
| Position tracking (`PositionTracker`) | P2 | ~50 lines |
| Output recovery manager (`OutputRecoveryManager`) | P2 | ~60 lines |
| Output worker signal interface (document, don't force) | P3 | ~0 lines |

### Keep Backend-Specific
| Component | Reason |
|-----------|--------|
| Device enumeration | APIs fundamentally different per platform |
| Format negotiation | APIs fundamentally different per platform |
| Exclusive mode | APIs fundamentally different per platform |
| Output worker internals | Event-driven vs polling vs callback |
| COM initialization | Windows-only |
| Registry access | Windows-only |
| ASIO driver management | Windows-only |
| Spatial audio flush | Windows-only |

### Factory Changes for macOS
```cpp
// audioplayerfactory.cpp additions needed:
#if defined(Q_OS_MACOS)
#include "coreaudioaudioplayer.h"
#endif

// In buildPlaybackPlan():
#elif defined(Q_OS_MACOS)
    if (CoreAudioAudioPlayer::isSupportedForContext(context, &reason)) {
        plan.backendId = AudioPlayerBackend::BackendId::CoreAudio;
        return plan;
    }

// In create():
case AudioPlayerBackend::BackendId::CoreAudio:
#if defined(Q_OS_MACOS)
    return new CoreAudioAudioPlayer(parent);
#else
    return new FfmpegAudioPlayer(parent);
#endif
```

---

## Implementation Roadmap

### Phase 6a — Shared Utilities (1-2 days)
1. Extract `PcmConverter` from WASAPI/ALSA workers
2. Extract `AudioLevelCalculator` from ALSA worker
3. Move `ScopedComInitializer` to shared (guarded)

### Phase 6b — ALSA Improvements (3-5 days)
1. Implement `snd_device_name_hint()` device enumeration
2. Add `plughw:` fallback chain
3. Add startup threshold and buffer generation tracking
4. Implement PCM fade-in/out

### Phase 6c — macOS CoreAudio Backend (5-10 days)
1. Create `src/backends/coreaudio/` directory
2. Implement `CoreAudioAudioPlayer` with `AudioUnit` render callback
3. Device enumeration via `AudioObjectGetPropertyData`
4. Format negotiation via `kAudioStreamPropertyPhysicalFormat`
5. Exclusive mode via hog mode
6. Factory integration
7. CMake platform conditions

### Phase 6d — Abstraction Polish (1-2 days)
1. Add `OutputRecoveryManager` to shared
2. Standardize diagnostic logging format across backends
3. Document backend signal contracts
