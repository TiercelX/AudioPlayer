# Phase 1: 播放流程完整调用链

## 一、公共入口阶段（所有后端共享）

### 1. 用户点击"打开文件"

```
[UI Thread]
MainWindow::openAudioFile()
  ├── QFileDialog::getOpenFileName()
  └── MainWindow::loadFileFromPath(filePath)
        ├── QFileInfo 校验文件存在
        ├── settings.setValue(kLastDirectoryKey, ...)
        └── MainWindow::loadAudioFile(filePath)
```

### 2. 加载音频文件 (MainWindow::loadAudioFile)

```
[UI Thread]
MainWindow::loadAudioFile(filePath)
  ├── m_player->stop()                              // 停止当前播放
  ├── resetMediaInfo()
  ├── setLoadingState(true, filePath)                // 显示加载进度UI
  ├── QFutureWatcher<AudioFileLoadResult>::setFuture(
  │     QtConcurrent::run(&prepareAudioFileForPlayback, filePath))
  │
  │   // ======== 异步执行（Worker Thread）========
  │   [Worker Thread]
  │   prepareAudioFileForPlayback(filePath)
  │     ├── PlaybackSourceService::probeSourceInfo(filePath)     ← 源探测
  │     │     ├── locateFfprobeExecutable()
  │     │     ├── runToolProcess(ffprobe, args, 3000ms, 5000ms)
  │     │     └── fallback: probeSourceInfoWithFfmpeg()
  │     │
  │     ├── AudioPlayerFactory::buildPlaybackPlan(sourceContext) ← 后端选择
  │     │     ├── 检查 .eb3/.ec3/.mlp → SourceMode::RemuxRawDolbySidecar
  │     │     ├── Windows: WindowsWasapiAudioPlayer::isSupportedForContext()
  │     │     ├── Linux:   LinuxAlsaAudioPlayer::isSupportedForContext()
  │     │     └── 返回 AudioPlaybackPlan { backendId, sourceMode }
  │     │
  │     └── PlaybackSourceService::resolveForPlayback()          ← 源准备
  │           ├── preparePlaybackSource(filePath, sourceMode)
  │           └── 返回 PlaybackSourceResolution { playbackPath, sourceInfo, durationMs }
  │
  │   // ======== 回到 UI Thread ========
  ├── replacePlayer(targetBackendId)                 ← 创建后端实例
  ├── m_player->setSource(playbackPath, ...)
  ├── updateDuration(m_probedDuration)
  └── m_player->play()                               ← 触发播放
```

---

## 二、FFmpeg 后端调用链 (FfmpegAudioPlayer)

### 架构概览

```
[UI Thread]          → MainWindow, FfmpegAudioPlayer (状态管理)
[m_audioThread]      → AudioOutputWorker (QAudioSink 驱动)
[m_decoderThread]    → FfmpegDecoderWorker (ffmpeg CLI 进程管理)
```

### 播放调用链

```
[UI Thread]
FfmpegAudioPlayer::play()
  └── (Stopped/Paused 无音频) → startPipeline(startPositionMs)

startPipeline(startPositionMs)
  ├── resolveOutputDevice()
  ├── selectOutputFormat()
  ├── teardownPipeline()                 ← 先拆除旧管线
  │     ├── releaseOutputResources()
  │     ├── stopDecoderWorker(true)
  │     └── clearBufferDevice()
  ├── new PcmStreamBuffer(this)          ← 创建 PCM 环形缓冲 (~3秒)
  ├── [audioThread] configureOutput()
  ├── 构建 ffmpeg 参数
  ├── setPlaybackState(Playing)
  └── [decoderThread] FfmpegDecoderWorker::startDecoding()
        └── QProcess::start(ffmpeg) → 循环 readAllStandardOutput → buffer->write()
```

### 数据流转

```
[Decoder Thread]                    [Audio Thread]
FfmpegDecoderWorker                 AudioOutputWorker
  │                                   │
  │ QProcess stdout → read            │
  │ buffer->write(pcmData)            │
  │   └── emit readyRead() ────────► │
  │                                   │ pumpOutput() (5ms timer)
  │                                   │   ├── buffer->read(bytesFree)
  │                                   │   └── m_outputDevice->write(chunk)
  │                                   │         └── QAudioSink → 系统音频输出
```

### Seek 操作

```
[UI Thread] seek(positionMs)
  └── (Playing) → startPipeline(clampedPosition)  ← 完整重建管线
```

---

## 三、WASAPI 后端调用链 (WindowsWasapiAudioPlayer)

### 架构概览

```
[UI Thread]          → WindowsWasapiAudioPlayer (状态管理, 事务协调)
[m_audioThread]      → WasapiOutputWorker (WASAPI COM 渲染循环)
[m_decoderThread]    → FfmpegDecoderWorker (ffmpeg CLI) 或 LibavSeekDecoderWorker (in-process)
```

### 播放调用链

```
[UI Thread]
WindowsWasapiAudioPlayer::play()
  └── startPipeline(startPositionMs, PipelineStartupProfile::NormalStart)

startPipeline(startPositionMs, startupProfile)
  ├── resolveOutputDevice()
  ├── selectOutputFormat()
  │     ├── exclusive模式: buildWaveFormat() + IAudioClient::IsFormatSupported()
  │     └── 返回 QAudioFormat, PcmStreamFormat, WAVEFORMATEX data
  ├── decoderFormatForOutput()
  ├── 判断解码器 (libav in-process vs ffmpeg CLI)
  ├── teardownPipeline()
  ├── performSpatialEndpointFlush()              ← 空间音频端点刷新
  ├── 创建 PcmStreamBuffer
  ├── [audioThread] WasapiOutputWorker::configureOutput()  [Blocking]
  │     ├── IMMDeviceEnumerator::GetDefaultAudioEndpoint()
  │     ├── IMMDevice::Activate(IAudioClient)
  │     ├── IAudioClient::Initialize(Shared/Exclusive)
  │     └── IAudioClient::GetService(IAudioRenderClient)
  ├── setPlaybackState(Playing)
  └── 启动解码 (LibavSeekDecoderWorker 或 FfmpegDecoderWorker)
```

### 数据流转

```
[Decoder Thread]                    [Audio Thread]
LibavSeekDecoderWorker /            WasapiOutputWorker
FfmpegDecoderWorker                    │
  │                                    │
  │ buffer->write(pcmData)             │
  │ emit dataAvailable() ────────────► │
  │                                    │ WaitForSingleObject(renderEvent)
  │ [UI Thread]                          ├── IAudioRenderClient->GetBuffer()
  │ startAudioOutputIfReady()            ├── buffer->read()
  │   └── IAudioClient->Start()          ├── 格式转换 + 音量调节
  │                                      ├── IAudioRenderClient->ReleaseBuffer()
  │                                      └── emit positionUpdated()
```

### Seek 操作

```
[UI Thread] seek(positionMs)
  └── (Playing) → startPipeline(clampedPosition, SeekResume)
        └── 使用 kSeekResume* 常量配置启动参数
```

### 错误恢复

```
StoppedState + IOError/FatalError
  └── shouldAttemptOutputRecovery()?
        ├── Yes → scheduleOutputRecovery()
        │     ├── attempt 1: 250ms delay
        │     ├── attempt 2: 500ms delay
        │     ├── attempt 3: 900ms delay
        │     └── startPipeline(positionMs, ErrorRecovery)
        └── No → teardownPipeline + emit errorOccurred
```

---

## 四、ASIO 后端调用链 (WindowsAsioAudioPlayer)

### 架构概览

```
[UI Thread]          → WindowsAsioAudioPlayer (状态管理)
[m_audioThread]      → AsioOutputWorker (ASIO 回调渲染)
[m_decoderThread]    → FfmpegDecoderWorker 或 LibavSeekDecoderWorker
[ASIO Driver Thread] → asioBufferSwitch() 回调
```

### 播放调用链

```
[UI Thread]
WindowsAsioAudioPlayer::play()
  └── startPipeline(startPositionMs)

startPipeline(startPositionMs)
  ├── 检测设备忙: AsioSessionProbe::hasActiveExternalWasapiRenderSessions()
  │     ├── 设备忙 → 重试 (500ms间隔, 30秒超时)
  │     └── 设备空闲 → continueStartPipeline()
  └── continueStartPipeline()
        ├── resolveOutputDevice() → 从注册表枚举 ASIO 设备
        ├── selectOutputFormat() → ASIO 驱动能力查询
        ├── teardownPipeline()
        ├── [audioThread] configureOutput() + prepareOutput()
        │     └── openDriver()
        │           ├── AsioDiscovery::createAsioDriver(clsid)
        │           ├── driver->init(hostWindow)
        │           ├── driver->createBuffers() → 注册 asioBufferSwitch 回调
        │           └── driver->getLatencies()
        ├── 启动解码
        └── setPlaybackState(Playing)
```

### 数据流转 (ASIO 回调)

```
[Decoder Thread]                    [ASIO Driver Thread]
FfmpegDecoderWorker /               ASIO 驱动
LibavSeekDecoderWorker                 │
  │                                    │
  │ buffer->write(pcmData)             │ asioBufferSwitch(doubleBufferIndex)
  │ emit dataAvailable() ────────────► │   └── g_callbackWorker->renderCallback(idx)
  │                                    │
  │ [UI Thread]                          │ AsioOutputWorker::renderCallback()
  │ startAudioOutputIfReady()            │   ├── buffer->readForOwner()
  │   └── driver->start()                │   ├── 格式转换 + 音量调节
  │                                      │   ├── memcpy → ASIO buffer
  │                                      │   └── emit positionUpdated()
```

### 暂停/恢复

```
pause()
  ├── driver->stop()                     ← 停止 ASIO 回调
  ├── decoderWorker->setPaused(true)
  └── m_pauseReleaseTimer->start(3000ms) ← 3秒后释放驱动

play() (恢复)
  ├── m_pauseReleaseTimer->stop()
  ├── decoderWorker->setPaused(false)
  └── resumeOutput()
        ├── 如果驱动已释放 → openDriver() + createBuffers()
        └── driver->start()
```

---

## 五、ALSA 后端调用链 (LinuxAlsaAudioPlayer)

### 架构概览

```
[UI Thread]          → LinuxAlsaAudioPlayer (状态管理)
[m_outputThread]     → AlsaOutputWorker (ALSA 写入循环)
[m_decoderThread]    → FfmpegDecoderWorker 或 LibavSeekDecoderWorker
```

### 播放调用链

```
[UI Thread]
LinuxAlsaAudioPlayer::play()
  └── startPipeline(startPositionMs)

startPipeline(startPositionMs)
  ├── selectOutputFormat()
  ├── openAlsaDevice(deviceId, exclusive)
  │     ├── 独占: snd_pcm_open("hw:0", PLAYBACK, 0)
  │     └── 共享: snd_pcm_open("default", PLAYBACK, 0)
  ├── AlsaFormatNegotiator::negotiate()
  │     └── 降级链路: hw: → plughw: → FFmpeg
  ├── snd_pcm_hw_params_set_*()
  ├── snd_pcm_prepare()
  ├── new PcmStreamBuffer + new AlsaOutputWorker
  ├── m_outputThread->start()
  │     └── AlsaOutputWorker::start() → run()
  └── 启动解码
```

### 数据流转 (ALSA 输出循环)

```
[Decoder Thread]                    [Output Thread]
FfmpegDecoderWorker /               AlsaOutputWorker::run()
LibavSeekDecoderWorker                 │
  │                                    │ while (m_running):
  │ buffer->write(pcmData)             │   ├── snd_pcm_avail_update()
  │                                    │   │   if < 0 → handleXrun()
  │                                    │   ├── buffer->readForOwner()
  │                                    │   ├── convertFormat()
  │                                    │   ├── volume 调节
  │                                    │   └── snd_pcm_writei()
  │                                    │         if < 0 → handleXrun()
```

### Seek 操作

```
[UI Thread] seek(positionMs)
  ├── teardownPipeline()                ← 完全拆除
  └── startPipeline(positionMs)         ← 从新位置重建
```

---

## 六、PcmStreamBuffer (共享缓冲层)

```
[Decoder Thread - 写入端]              [Audio Thread - 读取端]
PcmStreamBuffer::writeData()           PcmStreamBuffer::readData()
  ├── QMutexLocker                       ├── QMutexLocker
  ├── if (m_discardWrites) → 丢弃       ├── 环形读取: memcpy ← m_data[m_readPos]
  ├── 环形写入: memcpy → m_data         ├── m_bufferedBytes -= read
  └── emit readyRead()                   └── return read

特性:
  ├── 线程安全 (QMutex)
  ├── 双向通知 (readyRead signal)
  ├── 可丢弃模式 (setDiscardWrites) ← 用于 stop/切换时快速清理
  ├── 所有权跟踪 (ownerSessionId, bufferGeneration) ← 防止旧 session 写入
  └── 最大容量限制 (setMaxSize)
```

---

## 七、线程总结

| 线程 | FFmpeg | WASAPI | ASIO | ALSA |
|------|--------|--------|------|------|
| **UI Thread** | MainWindow + FfmpegAudioPlayer | MainWindow + WindowsWasapiAudioPlayer | MainWindow + WindowsAsioAudioPlayer | MainWindow + LinuxAlsaAudioPlayer |
| **Audio Thread** | AudioOutputWorker (QAudioSink) | WasapiOutputWorker (WASAPI COM) | AsioOutputWorker (ASIO callback) | AlsaOutputWorker (snd_pcm_writei) |
| **Decoder Thread** | FfmpegDecoderWorker (QProcess) | FfmpegDecoderWorker / LibavSeekDecoderWorker | FfmpegDecoderWorker / LibavSeekDecoderWorker | FfmpegDecoderWorker / LibavSeekDecoderWorker |
| **驱动线程** | - | - | ASIO Driver Thread | - |
