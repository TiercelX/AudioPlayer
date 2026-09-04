# Phase 1: 类依赖图

## 一、核心依赖关系

```
┌─────────────────────────────────────────────────────────────────┐
│                         UI Layer                                │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐     │
│  │  MainWindow   │  │MediaInfoDialog│  │AutomationOptions│     │
│  └──────┬───────┘  └──────────────┘  └──────────────────┘     │
│         │                                                       │
│         ├── uses ──▶ PlaybackSourceService                     │
│         ├── uses ──▶ AudioPlayerFactory                        │
│         └── owns ──▶ AudioPlayerBackend* (m_player)            │
└─────────────────────────────────────────────────────────────────┘
                              │
                              │ creates via AudioPlayerFactory
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                      Core Layer                                 │
│  ┌──────────────────┐  ┌──────────────────┐                    │
│  │AudioPlayerBackend │  │AudioPlayerFactory │                    │
│  │  (abstract base)  │  │  (static factory) │                    │
│  └────────┬─────────┘  └────────┬─────────┘                    │
│           │                      │                               │
│           │                      ├── uses ──▶ AudioPlayerBackend│
│           │                      └── uses ──▶ PlaybackSource... │
│           │                                                       │
│  ┌────────┴─────────────────────────────────────────────┐      │
│  │              Concrete Backends                        │      │
│  │  ┌────────────┐ ┌────────────┐ ┌────────────┐       │      │
│  │  │ FFmpeg     │ │ WASAPI     │ │ ASIO       │       │      │
│  │  │ Player     │ │ Player     │ │ Player     │       │      │
│  │  └────────────┘ └────────────┘ └────────────┘       │      │
│  │  ┌────────────┐                                     │      │
│  │  │ ALSA       │                                     │      │
│  │  │ Player     │                                     │      │
│  │  └────────────┘                                     │      │
│  └──────────────────────────────────────────────────────┘      │
└─────────────────────────────────────────────────────────────────┘
```

## 二、后端内部依赖

### FFmpeg 后端

```
FfmpegAudioPlayer
  ├── owns ──▶ QThread (m_audioThread)
  │     └── owns ──▶ AudioOutputWorker
  │           ├── uses ──▶ QAudioSink
  │           └── reads ──▶ PcmStreamBuffer
  ├── owns ──▶ QThread (m_decoderThread)
  │     └── owns ──▶ FfmpegDecoderWorker
  │           ├── uses ──▶ QProcess (ffmpeg)
  │           └── writes ──▶ PcmStreamBuffer
  └── owns ──▶ PcmStreamBuffer
```

### WASAPI 后端

```
WindowsWasapiAudioPlayer
  ├── owns ──▶ QThread (m_audioThread)
  │     └── owns ──▶ WasapiOutputWorker
  │           ├── uses ──▶ IMMDevice
  │           ├── uses ──▶ IAudioClient
  │           ├── uses ──▶ IAudioRenderClient
  │           ├── uses ──▶ IAudioStreamVolume
  │           ├── owns ──▶ AudioArtifactMonitor
  │           └── reads ──▶ PcmStreamBuffer
  ├── owns ──▶ QThread (m_decoderThread)
  │     ├── owns ──▶ FfmpegDecoderWorker
  │     │     ├── uses ──▶ QProcess (ffmpeg)
  │     │     └── writes ──▶ PcmStreamBuffer
  │     └── owns ──▶ LibavSeekDecoderWorker
  │           ├── uses ──▶ FFmpeg C API (libav*)
  │           ├── owns ──▶ DolbyDownmixProcessor
  │           ├── uses ──▶ PcmSeekCache
  │           └── writes ──▶ PcmStreamBuffer
  ├── owns ──▶ PcmStreamBuffer
  └── owns ──▶ PcmSeekCache
```

### ASIO 后端

```
WindowsAsioAudioPlayer
  ├── owns ──▶ QThread (m_audioThread)
  │     └── owns ──▶ AsioOutputWorker
  │           ├── uses ──▶ IASIO (ASIO driver)
  │           ├── owns ──▶ AudioArtifactMonitor
  │           └── reads ──▶ PcmStreamBuffer
  ├── owns ──▶ QThread (m_decoderThread)
  │     ├── owns ──▶ FfmpegDecoderWorker
  │     └── owns ──▶ LibavSeekDecoderWorker
  └── owns ──▶ PcmStreamBuffer

AsioDiscovery (static utility)
  ├── reads ──▶ Windows Registry
  └── creates ──▶ IASIO

AsioSessionProbe (static utility)
  └── uses ──▶ Windows Audio Session API

AsioUtils (static utility)
  └── wraps ──▶ ASIO API calls (with SEH)
```

### ALSA 后端

```
LinuxAlsaAudioPlayer
  ├── owns ──▶ QThread (m_outputThread)
  │     └── owns ──▶ AlsaOutputWorker
  │           ├── uses ──▶ snd_pcm_t
  │           └── reads ──▶ PcmStreamBuffer
  ├── owns ──▶ QThread (m_decoderThread)
  │     ├── owns ──▶ FfmpegDecoderWorker
  │     └── owns ──▶ LibavSeekDecoderWorker
  ├── owns ──▶ PcmStreamBuffer
  └── owns ──▶ PcmSeekCache

AlsaFormatNegotiator (static utility)
  └── uses ──▶ snd_pcm_hw_params API
```

## 三、共享组件依赖

```
PcmStreamBuffer (QIODevice)
  ├── thread-safe via QMutex
  ├── written by ──▶ FfmpegDecoderWorker / LibavSeekDecoderWorker
  └── read by ──▶ AudioOutputWorker / WasapiOutputWorker / AsioOutputWorker / AlsaOutputWorker

PcmSeekCache
  ├── thread-safe via QMutex
  ├── used by ──▶ LibavSeekDecoderWorker
  └── stores ──▶ PCM data (memory or disk)

AudioArtifactMonitor
  ├── used by ──▶ WasapiOutputWorker
  ├── used by ──▶ AsioOutputWorker
  └── analyzes ──▶ PCM blocks for pop/click/crackle

PlayerLogger (global)
  └── used by ──▶ all modules

AudioUtils (namespace)
  └── used by ──▶ all backends

ToolLocator (namespace)
  └── used by ──▶ PlaybackSourceService, FfmpegAudioPlayer
```

## 四、依赖方向总结

```
UI Layer
  │
  ▼
Core Layer (AudioPlayerBackend, AudioPlayerFactory, PlaybackSourceService)
  │
  ▼
Backend Layer (FFmpeg, WASAPI, ASIO, ALSA)
  │
  ├──▶ Shared Components (PcmStreamBuffer, PcmSeekCache, AudioArtifactMonitor)
  │
  └──▶ System APIs
        ├── Windows: COM (WASAPI), ASIO SDK, Registry
        ├── Linux: ALSA API
        └── Cross-platform: FFmpeg C API, QAudioSink
```

## 五、耦合度评估

| 组件 | 耦合度 | 说明 |
|------|--------|------|
| MainWindow | 中 | 依赖 AudioPlayerBackend 接口和 PlaybackSourceService |
| AudioPlayerBackend | 低 | 纯接口类，无具体实现依赖 |
| AudioPlayerFactory | 低 | 仅依赖 AudioPlayerBackend 接口 |
| PlaybackSourceService | 低 | 仅依赖 FFmpeg 工具进程 |
| 各后端实现 | 高 | 内部组件紧密耦合（Worker、Buffer、设备管理） |
| PcmStreamBuffer | 低 | 被所有后端使用，但自身无外部依赖 |
| AudioArtifactMonitor | 低 | 被 WASAPI/ASIO 使用，但自身无外部依赖 |
