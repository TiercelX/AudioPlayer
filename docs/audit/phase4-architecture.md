# Phase 4: 架构设计文档

## 1. 整体架构概述

AudioPlayer 是一个基于 Qt6 的跨平台音频播放器，采用模块化架构设计，支持多种音频后端。核心设计理念是**后端抽象 + 双 Worker 并行**，通过统一的 `AudioPlayerBackend` 接口屏蔽底层实现差异。

### 架构分层

```
┌─────────────────────────────────────────────────────────────┐
│                      UI 层 (MainWindow)                      │
├─────────────────────────────────────────────────────────────┤
│                    核心服务层 (Core)                          │
│  ┌──────────────┐  ┌──────────────────┐  ┌───────────────┐  │
│  │AudioPlayer   │  │PlaybackSource    │  │AudioPlayer    │  │
│  │Backend       │  │Service           │  │Factory        │  │
│  └──────────────┘  └──────────────────┘  └───────────────┘  │
├─────────────────────────────────────────────────────────────┤
│                    后端实现层 (Backends)                      │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐    │
│  │ FFmpeg   │  │ WASAPI   │  │  ASIO    │  │  ALSA    │    │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘    │
├─────────────────────────────────────────────────────────────┤
│                    共享组件层 (Shared)                        │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐  │
│  │PcmStream     │  │PcmSeekCache  │  │AudioArtifact     │  │
│  │Buffer        │  │              │  │Monitor           │  │
│  └──────────────┘  └──────────────┘  └──────────────────┘  │
├─────────────────────────────────────────────────────────────┤
│                    诊断层 (Diagnostics)                      │
│  ┌──────────────┐  ┌──────────────────────────────────────┐ │
│  │PlayerLogger  │  │DiagnosticReportBuilder               │ │
│  └──────────────┘  └──────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

---

## 2. 模块划分和职责

### 2.1 核心模块 (src/core/)

| 类名 | 文件 | 职责 |
|------|------|------|
| `AudioPlayerBackend` | `audioplayerbackend.h:25` | 所有后端的抽象基类，定义统一接口 |
| `AudioPlayerFactory` | `audioplayerfactory.h:28` | 后端选择与实例化工厂 |
| `PlaybackSourceService` | `playbacksourceservice.h:54` | 播放源探测、解析、缓存管理 |
| `NativeAudioPlayerStubBase` | `nativeaudioplayerstubbase.h` | 原生平台播放器桩基类 |

### 2.2 FFmpeg 后端模块 (src/backends/ffmpeg/)

| 类名 | 文件 | 职责 |
|------|------|------|
| `FfmpegAudioPlayer` | `ffmpegaudioplayer.h:16` | 基于 FFmpeg 进程管道 + QAudioSink 的跨平台后端 |
| `PcmStreamBuffer` | `ffmpegpcmshared.h:102` | 线程安全环形 PCM 缓冲区 |
| `FfmpegDecoderWorker` | `ffmpegpcmshared.h:153` | FFmpeg 解码工作线程 |
| `LibavSeekDecoderWorker` | `libavseekdecoderworker.h` | 基于 libav API 的带 seek 解码 Worker |
| `PcmSeekCache` | `pcmseekcache.h:12` | PCM 数据的 seek 缓存（内存/磁盘） |
| `DolbyDownmixProcessor` | `dolbydownmixprocessor.h` | Dolby 多声道→立体声下混处理器 |

### 2.3 WASAPI 后端模块 (src/backends/wasapi/)

| 类名 | 文件 | 职责 |
|------|------|------|
| `WindowsWasapiAudioPlayer` | `windowswasapiaudioplayer.h:21` | Windows WASAPI 后端，支持共享/独占模式 |
| `WasapiOutputWorker` | `windowswasapiaudioplayer_worker.h` | WASAPI 音频渲染 Worker |

### 2.4 ASIO 后端模块 (src/backends/asio/)

| 类名 | 文件 | 职责 |
|------|------|------|
| `WindowsAsioAudioPlayer` | `windowsasioaudioplayer.h:16` | Windows ASIO 后端 |
| `AsioOutputWorker` | `windowsasioaudioplayer_worker.h` | ASIO 音频输出 Worker |
| `AsioDiscovery` | `windowsasioaudioplayer_discovery.h` | ASIO 驱动发现 |
| `AsioSessionProbe` | `windowsasioaudioplayer_sessionprobe.h` | ASIO 会话探测 |

### 2.5 ALSA 后端模块 (src/backends/alsa/)

| 类名 | 文件 | 职责 |
|------|------|------|
| `LinuxAlsaAudioPlayer` | `linuxalsaaudioplayer.h:19` | Linux ALSA 后端 |
| `AlsaOutputWorker` | `alsaoutputworker.h` | ALSA 输出 Worker |
| `AlsaFormatNegotiator` | `alsaformatnegotiator.h` | ALSA 格式协商（hw: → plughw: → FFmpeg） |

### 2.6 诊断模块 (src/diagnostics/)

| 类名 | 文件 | 职责 |
|------|------|------|
| `AudioArtifactMonitor` | `audioartifactmonitor.h:11` | 音频伪影检测引擎 |
| `PlayerLogger` | `playerlogger.h` | 全局结构化日志接口 |

---

## 3. 数据流图

### 3.1 播放数据流

```
┌──────────────┐     ┌──────────────────┐     ┌──────────────────┐
│  源文件      │────▶│ PlaybackSource   │────▶│ AudioPlayer      │
│  (音频文件)  │     │ Service          │     │ Factory          │
└──────────────┘     │ - 探测格式       │     │ - 选择后端       │
                     │ - 准备 sidecar   │     │ - 创建实例       │
                     └──────────────────┘     └────────┬─────────┘
                                                       │
                                                       ▼
                                              ┌──────────────────┐
                                              │ AudioPlayer      │
                                              │ Backend (子类)    │
                                              └────────┬─────────┘
                                                       │
                         ┌─────────────────────────────┼─────────────────────────────┐
                         │                             │                             │
                         ▼                             ▼                             ▼
                ┌──────────────────┐         ┌──────────────────┐         ┌──────────────────┐
                │ Decoder Worker   │         │ PcmStreamBuffer  │         │ Output Worker    │
                │ (解码线程)       │────────▶│ (环形缓冲区)     │────────▶│ (音频输出线程)   │
                │                  │  写入   │                  │  读取   │                  │
                │ - FfmpegDecoder  │         │ - QMutex 保护    │         │ - QAudioSink     │
                │ - LibavSeek      │         │ - session/generation        │ - WasapiOutput   │
                │   Decoder        │         │   所有权验证     │         │ - AsioOutput     │
                └──────────────────┘         └──────────────────┘         │ - AlsaOutput     │
                                                                          └────────┬─────────┘
                                                                                   │
                                                                                   ▼
                                                                          ┌──────────────────┐
                                                                          │  音频设备输出     │
                                                                          │  (扬声器/耳机)   │
                                                                          └──────────────────┘
```

### 3.2 Seek 数据流

```
用户 Seek 请求
       │
       ▼
┌──────────────────┐     ┌──────────────────┐     ┌──────────────────┐
│ Backend.seek()   │────▶│ PcmSeekCache     │────▶│ 命中?            │
│                  │     │ - findHit()      │     │                  │
└──────────────────┘     └──────────────────┘     └────────┬─────────┘
                                                           │
                           ┌───────────────────────────────┼───────────────────────┐
                           │ 命中                          │ 未命中                │
                           ▼                               ▼                       │
                   ┌──────────────┐               ┌──────────────┐                │
                   │ 读取缓存     │               │ 重新解码     │                │
                   │ readSegment()│               │ seek + decode│                │
                   └──────┬───────┘               └──────┬───────┘                │
                          │                              │                        │
                          └──────────────┬───────────────┘                        │
                                         │                                        │
                                         ▼                                        │
                                 ┌──────────────┐                                 │
                                 │ 写入缓存     │◀────────────────────────────────┘
                                 │ writeSegment │
                                 └──────────────┘
```

---

## 4. 线程模型

### 4.1 双 Worker 架构

所有后端均采用**双 Worker 架构**：

```
┌─────────────────────────────────────────────────────────────────────┐
│                          主线程 (UI 线程)                            │
│  - 管理后端生命周期                                                  │
│  - 处理用户交互                                                      │
│  - 协调解码和输出                                                    │
└─────────────────────────────┬───────────────────────────────────────┘
                              │ Qt 信号/槽
        ┌─────────────────────┼─────────────────────┐
        │                     │                     │
        ▼                     │                     ▼
┌───────────────────┐         │         ┌───────────────────┐
│   解码线程        │         │         │   输出线程        │
│   (m_decoderThread)│        │         │   (m_audioThread) │
│                   │         │         │                   │
│ - FfmpegDecoder   │         │         │ - QAudioSink      │
│   Worker          │         │         │ - WasapiOutput    │
│ - LibavSeek       │         │         │   Worker          │
│   DecoderWorker   │         │         │ - AsioOutput      │
│                   │         │         │   Worker          │
│ 写入 PcmStream    │         │         │ - AlsaOutput      │
│ Buffer            │         │         │   Worker          │
└─────────┬─────────┘         │         └─────────┬─────────┘
          │                   │                   │
          └───────────────────┼───────────────────┘
                              │
                              ▼
                    ┌───────────────────┐
                    │ PcmStreamBuffer   │
                    │ (QMutex 保护)     │
                    └───────────────────┘
```

### 4.2 后端线程模型对比

| 后端 | 解码线程 | 输出线程 | 同步机制 |
|------|----------|----------|----------|
| FFmpeg | FfmpegDecoderWorker | AudioOutputWorker (QAudioSink) | PcmStreamBuffer (QMutex) + Qt 信号/槽 |
| WASAPI | FfmpegDecoderWorker / LibavSeekDecoderWorker | WasapiOutputWorker | PcmStreamBuffer (QMutex) + Windows 事件 + QWinEventNotifier |
| ASIO | FfmpegDecoderWorker / LibavSeekDecoderWorker | AsioOutputWorker (驱动回调) | PcmStreamBuffer (QMutex) + QMutex + std::atomic |
| ALSA | FfmpegDecoderWorker | AlsaOutputWorker | PcmStreamBuffer (QMutex) + Qt 信号/槽 |

### 4.3 线程安全设计

- **PcmStreamBuffer**: 使用 `mutable QMutex m_mutex` 保护所有读写操作 (`ffmpegpcmshared.h:139`)
- **PcmSeekCache**: 使用 `mutable QMutex m_mutex` 保护缓存访问 (`pcmseekcache.h:70`)
- **PlayerLogger**: 内部同步，全局线程安全
- **AudioArtifactMonitor**: 非线程安全，由输出 Worker 单线程使用

---

## 5. 后端选择策略

### 5.1 选择流程

```
AudioPlayerFactory::buildPlaybackPlan()
        │
        ▼
┌───────────────────────────────────────────────────┐
│ 检查源文件上下文                                    │
│ - 文件路径                                         │
│ - 编解码器名称 (codecName)                         │
│ - 声道数 (sourceChannelCount)                      │
└────────────────────────┬──────────────────────────┘
                         │
                         ▼
┌───────────────────────────────────────────────────┐
│ 判断是否需要 Dolby 重混                            │
│ - isPackagedLibavAudioCodec() 检查                 │
│ - 多声道 Dolby 编码 → SourceMode::RemuxRawDolbySidecar │
└────────────────────────┬──────────────────────────┘
                         │
                         ▼
┌───────────────────────────────────────────────────┐
│ 选择后端                                          │
│ 1. Windows + WASAPI 可用 → WindowsWasapi          │
│ 2. Windows + ASIO 可用 → WindowsAsio              │
│ 3. Linux + ALSA 可用 → LinuxAlsa                  │
│ 4. 其他平台 → Ffmpeg (跨平台后备)                  │
└───────────────────────────────────────────────────┘
```

### 5.2 后端特性对比

| 特性 | FFmpeg | WASAPI | ASIO | ALSA |
|------|--------|--------|------|------|
| 独占模式 | ✗ | ✓ | ✓ | ✓ |
| 空间音频 | ✗ | ✓ | ✗ | ✗ |
| 设备切换 | 基础 | 高级（事务状态机） | 基础 | 基础 |
| Seek 缓存 | ✗ | ✓ | ✓ | ✓ |
| Dolby 下混 | ✓ | ✓ | ✓ | ✓ |
| 平台 | 跨平台 | Windows | Windows | Linux |

---

## 6. 错误处理策略

### 6.1 错误恢复层次

```
┌─────────────────────────────────────────────────────────────┐
│                    错误恢复策略层次                           │
├─────────────────────────────────────────────────────────────┤
│ Level 1: 自动重试                                            │
│ - 输出设备临时不可用                                         │
│ - 缓冲区欠载 (XRUN)                                         │
│ - ASIO 驱动忙                                               │
├─────────────────────────────────────────────────────────────┤
│ Level 2: 输出重建                                            │
│ - WASAPI 设备失效 → 重新创建 IAudioClient                   │
│ - 输出格式不匹配 → 重新协商格式                              │
│ - ALSA 设备挂起 → 重新打开设备                               │
├─────────────────────────────────────────────────────────────┤
│ Level 3: 会话重建                                            │
│ - 解码进程崩溃 → 重启解码 Worker                             │
│ - 严重格式错误 → 重新初始化整个管线                          │
├─────────────────────────────────────────────────────────────┤
│ Level 4: 用户通知                                            │
│ - 不可恢复错误 → errorOccurred 信号                          │
│ - 设备不可用 → statusMessage 信号                            │
└─────────────────────────────────────────────────────────────┘
```

### 6.2 WASAPI 错误恢复状态机

```
┌──────────────┐    设备失效    ┌──────────────────┐
│   Playing    │──────────────▶│ WaitingFor       │
│              │               │ Invalidation     │
└──────────────┘               └────────┬─────────┘
       ▲                                │ 超时/确认
       │                                ▼
       │                       ┌──────────────────┐
       │                       │ Pending          │
       │                       │ - 保存播放位置   │
       │                       │ - 释放旧资源     │
       │                       └────────┬─────────┘
       │                                │
       │                                ▼
       │                       ┌──────────────────┐
       │                       │ Preflight        │
       │                       │ - 选择新设备     │
       │                       │ - 协商格式       │
       │                       └────────┬─────────┘
       │                                │
       │                                ▼
       │                       ┌──────────────────┐
       │                       │ Applying         │
       │                       │ - 创建新 Worker  │
       │                       │ - 恢复播放       │
       │                       └────────┬─────────┘
       │                                │
       └────────────────────────────────┘
```

### 6.3 错误信号传播

```cpp
// 错误信号链
Worker 线程错误
       │
       ▼
Backend::handleDecoderError() / handleAudioStateChanged()
       │
       ▼
emit errorOccurred(message)  // audioplayerbackend.h:131
       │
       ▼
MainWindow::handleError()    // 主线程处理
       │
       ▼
用户界面提示 / 自动恢复
```

### 6.4 日志与诊断

所有错误通过 `PlayerLogger` 记录，包含：
- 结构化文本日志（人类可读）
- JSONL 诊断日志（机器可解析）
- 音频伪影检测结果（AudioArtifactMonitor）

日志路径：`logs/` 目录，按时间戳 + PID 命名。
