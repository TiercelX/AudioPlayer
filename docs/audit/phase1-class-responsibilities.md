# Phase 1: 类职责分析

## 核心模块 (core)

### AudioPlayerBackend (audioplayerbackend.h)
- **基类**: QObject (抽象基类)
- **职责**: 所有音频播放后端的统一抽象接口，定义播放控制、设备管理、状态/信号契约
- **主要公共方法**: `play()`, `pause()`, `stop()`, `seek()`, `setVolume()`, `setSource()`, `availableOutputDevices()`, `outputFormat()`, `setOutputDeviceId()`, `refreshOutputConfiguration()`
- **信号**: `audioLevelsChanged`, `errorOccurred`, `finished`, `outputFormatChanged`, `playbackStateChanged`, `positionChanged`, `statusMessage`
- **枚举**: `BackendId` {Ffmpeg, WindowsWasapi, WindowsAsio, AppleNative, AndroidNative, LinuxAlsa}; `PlaybackState` {Stopped, Playing, Paused, Stopping}
- **线程模型**: 主线程接口类，具体线程行为由子类实现

### AudioPlayerFactory (audioplayerfactory.h)
- **基类**: 无 (纯静态工具类)
- **职责**: 根据源文件上下文选择后端并创建对应的 AudioPlayerBackend 实例
- **主要方法**: `buildPlaybackPlan()`, `selectBackend()`, `create()`
- **线程模型**: 无状态工具类，可在任意线程调用

### PlaybackSourceService (playbacksourceservice.h)
- **基类**: 无
- **职责**: 播放源的探测、解析、缓存管理和 sidecar（Dolby 重混）文件准备
- **主要方法**: `estimatePreparation()`, `probeSourceInfo()`, `probeDuration()`, `resolveForPlayback()`, `prunePlaybackCacheNow()`
- **线程模型**: 无状态服务类，同步调用（内部启动 QProcess 调用 ffprobe/ffmpeg）

### PlaybackSourceServiceInternal (playbacksourceserviceinternal.h)
- **基类**: 无 (namespace)
- **职责**: PlaybackSourceService 的内部辅助函数——进程执行、媒体信息格式化
- **线程模型**: 无状态工具函数，同步执行

### NativeAudioPlayerStubBase (nativeaudioplayerstubbase.h)
- **基类**: AudioPlayerBackend
- **职责**: 原生平台播放器的桩基类（Apple/Android），所有操作报告"不可用"
- **线程模型**: 主线程，纯桩实现

---

## FFmpeg 后端模块

### FfmpegAudioPlayer (ffmpegaudioplayer.h)
- **基类**: AudioPlayerBackend
- **职责**: 基于 FFmpeg 进程管道 + Qt QAudioSink 的跨平台音频播放后端
- **关键成员**: `QThread *m_audioThread`, `AudioOutputWorker *m_audioWorker`, `QThread *m_decoderThread`, `FfmpegDecoderWorker *m_decoderWorker`, `PcmStreamBuffer *m_bufferDevice`
- **线程模型**: **双 Worker 架构**。主线程管理生命周期；解码线程运行 FfmpegDecoderWorker（QProcess 调用 ffmpeg）；音频线程运行 AudioOutputWorker（QAudioSink 输出）

### PcmStreamFormat / PcmStreamBuffer / FfmpegDecoderWorker (ffmpegpcmshared.h)
- **PcmStreamFormat**: PCM 流格式描述（采样率、通道数、编码类型、有效位深）
- **PcmStreamBuffer**: 基于 QIODevice 的线程安全环形 PCM 缓冲区，`mutable QMutex m_mutex` 保护，支持 session/generation 所有权验证
- **FfmpegDecoderWorker**: 工作线程中通过 QProcess 运行 ffmpeg 解码，将 PCM 写入 PcmStreamBuffer
- **线程模型**: 生产者-消费者模式，解码线程写入，音频输出线程读取

### DolbyDownmixProcessor (dolbydownmixprocessor.h)
- **基类**: 无
- **职责**: Dolby 下混处理器，多声道（5.1/7.1）→ 立体声，支持 LoRo/LtRt/DplII 模式
- **线程模型**: 非线程安全，由 LibavSeekDecoderWorker 在其工作线程中使用

### LibavSeekDecoderWorker (libavseekdecoderworker.h)
- **基类**: QObject
- **职责**: 基于 libav API（FFmpeg C 库）的带 seek 能力的解码 Worker，支持预准备源、快速 seek、PCM 缓存
- **关键成员**: `QTimer *m_decodeTimer`, `PcmStreamBuffer *m_buffer`, `PcmSeekCache *m_seekCache`, 内含 `DolbyDownmixProcessor`
- **线程模型**: 运行在独立 QThread，通过信号与主线程通信

### PcmSeekCache (pcmseekcache.h)
- **基类**: 无
- **职责**: PCM 数据的 seek 缓存，支持内存和磁盘两种存储模式，加速重复 seek
- **线程安全**: `mutable QMutex m_mutex`
- **存储模式**: Memory / Disk / Disabled

---

## WASAPI 后端模块

### WindowsWasapiAudioPlayer (windowswasapiaudioplayer.h)
- **基类**: AudioPlayerBackend
- **职责**: Windows WASAPI 音频播放后端，支持共享/独占模式、活动输出切换事务、错误恢复、空间音频
- **关键枚举**: `ActiveOutputSwitchTrigger`, `ActiveOutputSwitchPhase`, `PipelineStartupProfile`, `ActiveOutputSwitchTransaction`
- **关键成员**: `QThread *m_audioThread` + `WasapiOutputWorker`, `QThread *m_decoderThread` + `FfmpegDecoderWorker`/`LibavSeekDecoderWorker`, `PcmSeekCache *m_pcmSeekCache`
- **线程模型**: **双 Worker 架构** + 复杂的活动输出切换事务状态机

### WasapiOutputWorker (windowswasapiaudioplayer_worker.h)
- **基类**: QObject
- **职责**: WASAPI 音频渲染 Worker，管理 IAudioClient/IAudioRenderClient 生命周期、PCM 提交、音量/淡入淡出、噪声整形
- **关键成员**: `IMMDevice *m_device`, `IAudioClient *m_audioClient`, `IAudioRenderClient *m_renderClient`, `IAudioStreamVolume *m_streamVolume`, `HANDLE m_refillEvent`, `QWinEventNotifier *m_eventNotifier`, `AudioArtifactMonitor m_artifactMonitor`
- **线程模型**: 运行在 m_audioThread，通过 Windows 事件 + QWinEventNotifier 驱动渲染循环

### Worker Helpers (windowswasapiaudioplayer_worker_helpers.h)
- **职责**: WASAPI Worker 的辅助常量、结构体和工具函数
- **关键常量**: `kRecoveryStartupSilenceMs`, `kPcmFadeInDurationMs`, `kExclusiveBufferDuration` 等
- **关键函数**: `buildWaveFormat()`, `mapWasapiError()`, `noiseShapedQuantize32To24/16()`, `creativeWasapiChannelOrderFilter()`

---

## ASIO 后端模块

### WindowsAsioAudioPlayer (windowsasioaudioplayer.h)
- **基类**: AudioPlayerBackend
- **职责**: Windows ASIO 音频播放后端，支持 ASIO 驱动直连、会话探测、驱动忙重试
- **关键成员**: `QThread *m_audioThread` + `AsioOutputWorker`, `QThread *m_decoderThread` + `FfmpegDecoderWorker`/`LibavSeekDecoderWorker`
- **线程模型**: **双 Worker 架构**。ASIO 回调在驱动线程触发，通过全局 `g_callbackWorker` 转发

### AsioOutputWorker (windowsasioaudioplayer_worker.h)
- **基类**: QObject
- **职责**: ASIO 音频输出 Worker，管理 ASIO 驱动生命周期、双缓冲渲染回调、格式转换和噪声整形
- **关键成员**: `IASIO *m_driver`, `QMutex m_renderMutex`, `std::atomic<long> m_callbackCount`, `AudioArtifactMonitor m_artifactMonitor`
- **线程模型**: **三线程交互**。主线程配置/启动/停止；ASIO 驱动线程触发回调；Worker 线程处理状态信号

### AsioDiscovery (windowsasioaudioplayer_discovery.h)
- **职责**: ASIO 驱动发现——注册表读取、宿主窗口查找、驱动实例创建
- **线程模型**: 主线程，涉及 COM 和注册表操作

### AsioFormats (windowsasioaudioplayer_formats.h)
- **职责**: ASIO 格式工具——PCM 编码名称、格式转换、采样率候选列表
- **线程模型**: 无状态工具函数

### AsioSessionProbe (windowsasioaudioplayer_sessionprobe.h)
- **职责**: ASIO 会话探测——WASAPI 端点占用检查、多物理设备检测
- **线程模型**: 主线程，涉及 Windows 音频会话枚举 API

### AsioUtils (windowsasioaudioplayer_utils.h)
- **职责**: ASIO 底层工具——安全的 ASIO API 包装（带 SEH 崩溃检测）
- **线程模型**: 无状态工具函数

---

## ALSA 后端模块

### LinuxAlsaAudioPlayer (linuxalsaaudioplayer.h)
- **基类**: AudioPlayerBackend
- **职责**: Linux ALSA 音频播放后端，支持独占/共享模式、格式协商降级链路
- **关键成员**: `QThread *m_decoderThread` + `FfmpegDecoderWorker`, `QThread *m_outputThread` + `AlsaOutputWorker`, `snd_pcm_t *m_pcmHandle`, `PcmSeekCache *m_pcmSeekCache`
- **线程模型**: **双 Worker 架构**（与 WASAPI/FFmpeg 一致）

### AlsaOutputWorker (alsaoutputworker.h)
- **基类**: QObject
- **职责**: ALSA 输出 Worker，从 PcmStreamBuffer 读取 PCM，格式转换后写入 ALSA 设备，处理 XRUN
- **关键成员**: `snd_pcm_t *m_handle`, `PcmStreamBuffer *m_buffer`, `m_xrunCount`
- **线程模型**: 运行在 m_outputThread，内含 run() 循环

### AlsaFormatNegotiator (alsaformatnegotiator.h)
- **职责**: ALSA 格式协商——降级链路 hw: → plughw: → FFmpeg
- **线程模型**: 无状态工具类

---

## 共享模块

### AudioUtils (audioutils.h)
- **职责**: 跨后端共享的音频工具——状态名称、通道布局、PCM 编码名称映射
- **线程模型**: 全部 inline，无状态，线程安全

### ToolLocator (toollocator.h)
- **职责**: 定位 ffmpeg/ffprobe 可执行文件路径
- **线程模型**: 无状态工具函数

---

## 诊断模块

### AudioArtifactMonitor (audioartifactmonitor.h)
- **基类**: 无
- **职责**: 音频伪影（pop/click/crackle）检测引擎，分析 PCM 块的峰值、RMS、跳变、爆裂纹理
- **线程模型**: 非线程安全，由各后端 Worker 在输出线程中单线程使用

### PlayerLogger (playerlogger.h)
- **职责**: 全局日志接口——结构化文本日志和 JSONL 诊断日志
- **线程模型**: 全局可用，被所有模块广泛使用

---

## UI 模块

### MainWindow (mainwindow.h)
- **基类**: QMainWindow
- **职责**: 应用主窗口，协调播放控制、输出设备管理、媒体信息显示、自动化接口
- **关键成员**: `AudioPlayerBackend *m_player`, `PlaybackSourceService m_playbackSourceService`
- **线程模型**: 主线程（UI 线程），后端 Worker 线程通过信号/槽与主线程通信

### MainWindowHelpers (mainwindow_helpers.h)
- **职责**: MainWindow 的辅助常量和格式化函数
- **线程模型**: 无状态工具函数

### MediaInfoDialog (mediainfodialog.h)
- **基类**: QDialog
- **职责**: 媒体信息对话框，显示源/输出的编解码格式、通道、采样率、位深、码率
- **线程模型**: 主线程 UI 对话框

### AutomationOptions / DiagnosticReportBuilder
- **职责**: CLI 自动化选项处理 / 诊断报告生成
- **线程模型**: 主线程调用

---

## 线程架构总览

| 模块 | 线程模型 | 关键同步机制 |
|------|----------|-------------|
| FFmpeg 后端 | 主线程 + 解码线程 + 音频输出线程 | PcmStreamBuffer (QMutex)、Qt 信号/槽 |
| WASAPI 后端 | 主线程 + 解码线程 + WASAPI 渲染线程 | PcmStreamBuffer (QMutex)、Windows 事件、QWinEventNotifier、Qt 信号/槽 |
| ASIO 后端 | 主线程 + 解码线程 + ASIO 驱动回调线程 | PcmStreamBuffer (QMutex)、QMutex、std::atomic、Qt 信号/槽 (QueuedConnection) |
| ALSA 后端 | 主线程 + 解码线程 + ALSA 输出线程 | PcmStreamBuffer (QMutex)、Qt 信号/槽 |
| PcmSeekCache | 被解码线程使用 | QMutex |
| AudioArtifactMonitor | 被输出 Worker 使用 | 无锁（单线程使用） |
| PlayerLogger | 全局所有线程 | 内部同步 |
| UI (MainWindow) | 主线程 | Qt 事件循环、信号/槽跨线程 |
