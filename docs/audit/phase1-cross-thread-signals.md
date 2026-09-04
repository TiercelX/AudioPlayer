# Phase 1: 跨线程信号槽分析

## 线程模型概述

所有后端共享双 Worker 线程架构:
- **主线程 (UI)**: MainWindow, AudioPlayerBackend 实例
- **音频线程 (m_audioThread)**: WasapiOutputWorker / AsioOutputWorker / AudioOutputWorker
- **解码线程 (m_decoderThread)**: FfmpegDecoderWorker / LibavSeekDecoderWorker

---

## 一、信号槽连接清单

### WASAPI 后端 (windowswasapiaudioplayer.cpp:46-122)

| # | 信号 | 接收方 | 连接类型 | 发送→接收线程 | 安全 |
|---|------|--------|----------|---------------|------|
| 1 | QThread::finished | m_audioWorker::deleteLater | Auto | 主→音频 | 安全 (queued) |
| 2 | WasapiOutputWorker::positionUpdated | handleAudioPositionUpdated | Auto | 音频→主 | 安全 (queued) |
| 3 | WasapiOutputWorker::stateChanged | handleAudioStateChanged | Auto | 音频→主 | 安全 (queued) |
| 4 | QThread::finished | m_decoderWorker::deleteLater | Auto | 主→解码 | 安全 (queued) |
| 5 | FfmpegDecoderWorker::dataAvailable | handleDecoderDataAvailable | Auto | 解码→主 | 安全 (queued) |
| 6 | FfmpegDecoderWorker::audioLevelsChanged | lambda | Auto | 解码→主 | 安全 (queued) |
| 7 | FfmpegDecoderWorker::errorOccurred | handleDecoderError | Auto | 解码→主 | 安全 (queued) |
| 8 | FfmpegDecoderWorker::finished | handleDecoderFinished | Auto | 解码→主 | 安全 (queued) |
| 9 | QThread::finished | m_libavSeekDecoderWorker::deleteLater | Auto | 主→解码 | 安全 (queued) |
| 10 | LibavSeekDecoderWorker::dataAvailable | handleDecoderDataAvailable | Auto | 解码→主 | 安全 (queued) |
| 11 | LibavSeekDecoderWorker::audioLevelsChanged | lambda | Auto | 解码→主 | 安全 (queued) |
| 12 | LibavSeekDecoderWorker::errorOccurred | handleDecoderError | Auto | 解码→主 | 安全 (queued) |
| 13 | LibavSeekDecoderWorker::finished | handleDecoderFinished | Auto | 解码→主 | 安全 (queued) |
| 14 | QTimer::timeout (m_outputDeviceChangeTimer) | lambda | Auto | 主→主 | 安全 (direct) |
| 15 | QMediaDevices::audioOutputsChanged | handleAudioOutputsChanged | Auto | 系统→主 | 安全 (queued) |

### WASAPI Worker 内部 (windowswasapiaudioplayer_worker.h:60-80)

| # | 信号 | 接收方 | 连接类型 | 线程 | 安全 |
|---|------|--------|----------|------|------|
| 16 | QTimer::timeout (m_positionTimer) | lambda | Auto | 音频内 | 安全 (direct) |
| 17 | QTimer::timeout (m_fadeTimer) | lambda | Auto | 音频内 | 安全 (direct) |
| 18 | QIODevice::readyRead | renderAvailableFrames | **Queued** | 解码→音频 | 安全 (queued) |

### ASIO 后端 (windowsasioaudioplayer.cpp:239-305)

| # | 信号 | 接收方 | 连接类型 | 发送→接收线程 | 安全 |
|---|------|--------|----------|---------------|------|
| 19 | QTimer::timeout (m_pauseReleaseTimer) | lambda | Auto | 主→主 | 安全 |
| 20 | QThread::finished | m_audioWorker::deleteLater | Auto | 主→音频 | 安全 |
| 21 | AsioOutputWorker::firstBufferSwitchReceived | handleAudioFirstBufferSwitch | Auto | 音频→主 | 安全 |
| 22 | AsioOutputWorker::positionUpdated | handleAudioPositionUpdated | Auto | 音频→主 | 安全 |
| 23 | AsioOutputWorker::stateChanged | handleAudioStateChanged | Auto | 音频→主 | 安全 |
| 24 | AsioOutputWorker::statusMessage | lambda | Auto | 音频→主 | 安全 |
| 25-32 | FfmpegDecoderWorker/LibavSeekDecoderWorker::* | 对应 handler | Auto | 解码→主 | 安全 |

### FFmpeg 后端 (ffmpegaudioplayer.cpp:467-504)

| # | 信号 | 接收方 | 连接类型 | 发送→接收线程 | 安全 |
|---|------|--------|----------|---------------|------|
| 33 | QThread::finished | m_audioWorker::deleteLater | Auto | 主→音频 | 安全 |
| 34 | AudioOutputWorker::positionUpdated | handleAudioPositionUpdated | Auto | 音频→主 | 安全 |
| 35 | AudioOutputWorker::stateChanged | handleAudioStateChanged | Auto | 音频→主 | 安全 |
| 36-39 | FfmpegDecoderWorker::* | 对应 handler | Auto | 解码→主 | 安全 |
| 40 | QMediaDevices::audioOutputsChanged | handleAudioOutputsChanged | Auto | 系统→主 | 安全 |

### FFmpeg Worker 内部

| # | 信号 | 连接类型 | 线程 |
|---|------|----------|------|
| 41 | m_pumpTimer::timeout → pumpOutput | Auto | 音频内 |
| 42 | m_positionTimer::timeout → lambda | Auto | 音频内 |
| 43 | m_volumeRampTimer::timeout → lambda | Auto | 音频内 |
| 44 | m_audioSink::stateChanged → lambda | Auto | 音频内 |
| 45 | m_buffer::readyRead → pumpOutput | **Queued** | 解码→音频 |

### ALSA 后端 (linuxalsaaudioplayer.cpp:10-23)

| # | 信号 | 接收方 | 连接类型 | 线程 | 安全 |
|---|------|--------|----------|------|------|
| 46 | QMediaDevices::audioOutputsChanged | lambda (启动 timer) | Auto | 系统→主 | 安全 |
| 47 | m_outputDeviceChangeTimer::timeout | handleAudioOutputsChanged | Auto | 主→主 | 安全 |

### MainWindow 信号 (mainwindow.cpp:349-518)

| # | 信号 | 接收方 | 连接类型 | 线程 |
|---|------|--------|----------|------|
| 48-65 | AudioPlayerBackend::* | 对应 lambda/handler | Auto | 后端→主 |
| 66 | QFutureWatcher::finished | lambda | Auto | Worker→主 |

---

## 二、QMetaObject::invokeMethod 跨线程调用

### WASAPI 后端 (BlockingQueuedConnection)

| # | 调用位置 | 目标 | 方法 | 类型 | 发送→接收 | 死锁风险 |
|---|----------|------|------|------|-----------|----------|
| 1 | 析构:128 | m_audioWorker | releaseOutput | **BlockingQueued** | 主→音频 | **高** |
| 2 | 析构:137 | m_decoderWorker | stopDecoding | **BlockingQueued** | 主→解码 | **高** |
| 3 | 析构:142 | m_libavSeekDecoderWorker | stopDecoding+releaseSource | **BlockingQueued** | 主→解码 | **高** |
| 4 | play():348 | m_audioWorker | resumeOutput | Queued | 主→音频 | 无 |
| 5 | pause():378 | m_audioWorker | pauseOutput | Queued | 主→音频 | 无 |
| 6 | releaseOutputResources():501 | m_audioWorker | releaseOutput | **BlockingQueued** | 主→音频 | **中** |
| 7 | prepareOutputDeviceChange():517 | m_audioWorker | prepareForOutputDeviceChange | **BlockingQueued** | 主→音频 | **中** |
| 8 | restoreOutputDeviceChange():530 | m_audioWorker | restoreAfterCancelled... | **BlockingQueued** | 主→音频 | **中** |
| 9 | prepareActiveOutputInvalidation():543 | m_audioWorker | prepareForActiveOutputInvalidation | **BlockingQueued** | 主→音频 | **中** |
| 10 | restoreActiveOutputInvalidation():556 | m_audioWorker | restoreAfterCancelled... | **BlockingQueued** | 主→音频 | **中** |
| 11 | performSpatialEndpointFlush():654 | m_audioWorker | flushSpatialEndpoint | Queued | 主→音频 | 无 |
| 12 | syncExclusiveModeState():687 | m_audioWorker | exclusiveModeActive | **BlockingQueued** | 主→音频 | **低** |
| 13 | reconfigureActiveOutput():772 | m_audioWorker | configureOutput | **BlockingQueued** | 主→音频 | **低** |
| 14 | reconfigureActiveOutput():812 | m_audioWorker | startOutput | **BlockingQueued** | 主→音频 | **低** |
| 15 | startAudioOutputIfReady():924 | m_audioWorker | startOutput | **BlockingQueued** | 主→音频 | **低** |
| 16 | startPipeline():1225 | m_audioWorker | configureOutput | **BlockingQueued** | 主→音频 | **低** |
| 17 | startPipeline():1386 | m_libavSeekDecoderWorker | setSeekCache | **BlockingQueued** | 主→解码 | **低** |
| 18 | startPipeline():1392 | m_libavSeekDecoderWorker | startDecoding | Queued | 主→解码 | 无 |
| 19 | startPipeline():1415 | m_decoderWorker | startDecoding | Queued | 主→解码 | 无 |
| 20 | stopDecoderWorker():868 | m_decoderWorker | stopDecoding | **BlockingQueued**/Queued | 主→解码 | **有** |
| 21 | stopDecoderWorker():875 | m_libavSeekDecoderWorker | stopDecoding | **BlockingQueued**/Queued | 主→解码 | **有** |
| 22 | setVolume():485 | m_audioWorker | setVolume | Queued | 主→音频 | 无 |

---

## 三、死锁风险评估

### 高风险场景

| 场景 | 代码位置 | 原因 |
|------|----------|------|
| 析构函数 BlockingQueued 调用 | `windowswasapiaudioplayer.cpp:128-146` | 如果 Worker 线程卡在 COM 调用，主线程会无限阻塞 |
| stopDecoderWorker(waitForFinished=true) | `windowswasapiaudioplayer.cpp:868-880` | 如果解码器卡在 ffmpeg 进程，主线程会阻塞 |

### 中风险场景

| 场景 | 代码位置 | 原因 |
|------|----------|------|
| releaseOutputResources | `windowswasapiaudioplayer.cpp:501-508` | 如果音频线程卡在 COM 调用 |
| prepareOutputDeviceChange | `windowswasapiaudioplayer.cpp:517` | 如果音频线程忙 |
| prepareActiveOutputInvalidation | `windowswasapiaudioplayer.cpp:543` | 如果音频线程忙 |

### 缓解措施

1. **析构函数**: 先 teardownPipeline 释放大部分资源，再 BlockingQueued 调用最终释放
2. **Worker 线程无反向阻塞**: Worker 线程中没有 BlockingQueued 调用回主线程（只有信号/Queued invokeMethod）
3. **COM 超时保护**: 音频线程的 COM 操作有超时保护
4. **唯一真正的双向阻塞风险**: 如果音频线程在 `flushSpatialEndpoint` 中的 `QThread::msleep()` 期间，主线程调用析构函数的 BlockingQueuedConnection，会阻塞直到 msleep 返回（上限 1000ms）

---

## 四、安全评估总结

| 连接类型 | 数量 | 安全性 |
|----------|------|--------|
| Auto (同线程) | ~30 | 安全 (direct) |
| Auto (跨线程) | ~40 | 安全 (自动 queued) |
| Queued (显式) | ~5 | 安全 |
| BlockingQueued | ~15 | 有死锁风险，但有缓解措施 |

**结论**: 所有信号槽连接都是线程安全的。BlockingQueuedConnection 存在理论上的死锁风险，但通过超时保护和无反向阻塞设计得到了有效缓解。
