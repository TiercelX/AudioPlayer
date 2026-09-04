# Phase 1: 内存管理分析

## 一、智能指针使用情况

### 已使用智能指针的位置

| 文件 | 类型 | 用途 |
|------|------|------|
| PcmStreamBuffer | QIODevice (Qt 对象树) | 由 QObject parent 管理生命周期 |
| QThread | Qt 对象树 | 由 parent QObject 管理 |
| QTimer | Qt 对象树 | 由 parent QObject 管理 |
| QMediaDevices | Qt 对象树 | 由 parent QObject 管理 |

### 未使用智能指针的位置

| 文件 | 类型 | 风险评估 |
|------|------|----------|
| WindowsWasapiAudioPlayer | `WasapiOutputWorker *m_audioWorker` | 中风险 - 析构函数中 BlockingQueued 释放 |
| WindowsWasapiAudioPlayer | `FfmpegDecoderWorker *m_decoderWorker` | 中风险 - 析构函数中 BlockingQueued 释放 |
| WindowsWasapiAudioPlayer | `LibavSeekDecoderWorker *m_libavSeekDecoderWorker` | 中风险 - 析构函数中 BlockingQueued 释放 |
| WindowsWasapiAudioPlayer | `PcmSeekCache *m_pcmSeekCache` | 低风险 - 析构函数中 delete |
| WindowsAsioAudioPlayer | `AsioOutputWorker *m_audioWorker` | 中风险 - 同 WASAPI |
| WindowsAsioAudioPlayer | `FfmpegDecoderWorker *m_decoderWorker` | 中风险 - 同 WASAPI |
| WindowsAsioAudioPlayer | `LibavSeekDecoderWorker *m_libavSeekDecoderWorker` | 中风险 - 同 WASAPI |
| FfmpegAudioPlayer | `AudioOutputWorker *m_audioWorker` | 中风险 - 同 WASAPI |
| FfmpegAudioPlayer | `FfmpegDecoderWorker *m_decoderWorker` | 中风险 - 同 WASAPI |
| LinuxAlsaAudioPlayer | `AlsaOutputWorker *m_outputWorker` | 中风险 - 同 WASAPI |
| LinuxAlsaAudioPlayer | `FfmpegDecoderWorker *m_decoderWorker` | 中风险 - 同 WASAPI |
| LinuxAlsaAudioPlayer | `LibavSeekDecoderWorker *m_libavSeekDecoderWorker` | 中风险 - 同 WASAPI |
| LinuxAlsaAudioPlayer | `PcmSeekCache *m_pcmSeekCache` | 低风险 - 析构函数中 delete |
| WasapiOutputWorker | `IMMDevice *m_device` | 低风险 - SafeRelease 释放 |
| WasapiOutputWorker | `IAudioClient *m_audioClient` | 低风险 - SafeRelease 释放 |
| WasapiOutputWorker | `IAudioRenderClient *m_renderClient` | 低风险 - SafeRelease 释放 |
| WasapiOutputWorker | `IAudioStreamVolume *m_streamVolume` | 低风险 - SafeRelease 释放 |
| WasapiOutputWorker | `HANDLE m_refillEvent` | 低风险 - CloseHandle 释放 |
| AsioOutputWorker | `IASIO *m_driver` | 低风险 - driver->release() 释放 |
| MainWindow | `AudioPlayerBackend *m_player` | 低风险 - replacePlayer() 中管理 |
| MainWindow | `MediaInfoDialog *m_mediaInfoDialog` | 低风险 - Qt 对象树管理 |

---

## 二、内存泄漏风险评估

### 低风险区域

1. **Qt 对象树管理的对象** (QThread, QTimer, QMediaDevices)
   - 由 parent QObject 自动释放
   - 风险: 极低

2. **COM 对象** (IMMDevice, IAudioClient, IAudioRenderClient)
   - 使用 SafeRelease() 模式
   - 在 releaseOutput() 和析构函数中释放
   - 风险: 低

3. **ASIO 驱动** (IASIO)
   - 使用 driver->release() 释放
   - 在 forceReleaseDriver() 和析构函数中释放
   - 风险: 低

### 中风险区域

1. **Worker 线程对象** (WasapiOutputWorker, FfmpegDecoderWorker 等)
   - 使用 BlockingQueuedConnection 在析构函数中释放
   - 风险: 如果 BlockingQueued 死锁，可能导致泄漏
   - 缓解: 先 teardownPipeline 释放大部分资源

2. **PcmStreamBuffer**
   - 使用 Qt 对象树管理
   - 但在 teardownPipeline 中通过 quarantineBufferDevice() 隔离
   - 风险: 低，但 quarantine 逻辑复杂

3. **PcmSeekCache**
   - 使用裸指针 delete
   - 风险: 低，但如果有异常可能泄漏

### 高风险区域

1. **ASIO 驱动回调中的全局指针** (g_callbackWorker)
   - 使用全局 raw 指针
   - 风险: 如果 Worker 被销毁但回调仍然触发，可能导致悬空指针
   - 缓解: 在析构函数中清除全局指针

2. **WASAPI 事件句柄** (m_refillEvent)
   - 使用 Windows HANDLE
   - 风险: 低，但在错误路径中可能遗漏 CloseHandle
   - 缓解: 在 releaseOutput() 中统一释放

---

## 三、Raw Pointer 使用清单

### 后端类中的 Raw Pointer

| 类 | 指针 | 类型 | 释放方式 | 风险 |
|----|------|------|----------|------|
| WindowsWasapiAudioPlayer | m_audioWorker | WasapiOutputWorker* | deleteLater via QThread::finished | 低 |
| WindowsWasapiAudioPlayer | m_decoderWorker | FfmpegDecoderWorker* | deleteLater via QThread::finished | 低 |
| WindowsWasapiAudioPlayer | m_libavSeekDecoderWorker | LibavSeekDecoderWorker* | deleteLater via QThread::finished | 低 |
| WindowsWasapiAudioPlayer | m_pcmSeekCache | PcmSeekCache* | delete in teardownPipeline | 低 |
| WindowsAsioAudioPlayer | m_audioWorker | AsioOutputWorker* | deleteLater via QThread::finished | 低 |
| WindowsAsioAudioPlayer | m_decoderWorker | FfmpegDecoderWorker* | deleteLater via QThread::finished | 低 |
| WindowsAsioAudioPlayer | m_libavSeekDecoderWorker | LibavSeekDecoderWorker* | deleteLater via QThread::finished | 低 |
| FfmpegAudioPlayer | m_audioWorker | AudioOutputWorker* | deleteLater via QThread::finished | 低 |
| FfmpegAudioPlayer | m_decoderWorker | FfmpegDecoderWorker* | deleteLater via QThread::finished | 低 |
| LinuxAlsaAudioPlayer | m_outputWorker | AlsaOutputWorker* | deleteLater via QThread::finished | 低 |
| LinuxAlsaAudioPlayer | m_decoderWorker | FfmpegDecoderWorker* | deleteLater via QThread::finished | 低 |
| LinuxAlsaAudioPlayer | m_libavSeekDecoderWorker | LibavSeekDecoderWorker* | deleteLater via QThread::finished | 低 |
| LinuxAlsaAudioPlayer | m_pcmSeekCache | PcmSeekCache* | delete in teardownPipeline | 低 |

### Worker 类中的 Raw Pointer

| 类 | 指针 | 类型 | 释放方式 | 风险 |
|----|------|------|----------|------|
| WasapiOutputWorker | m_device | IMMDevice* | SafeRelease | 低 |
| WasapiOutputWorker | m_audioClient | IAudioClient* | SafeRelease | 低 |
| WasapiOutputWorker | m_renderClient | IAudioRenderClient* | SafeRelease | 低 |
| WasapiOutputWorker | m_streamVolume | IAudioStreamVolume* | SafeRelease | 低 |
| WasapiOutputWorker | m_refillEvent | HANDLE | CloseHandle | 低 |
| AsioOutputWorker | m_driver | IASIO* | driver->release() | 低 |

### 全局 Raw Pointer

| 指针 | 类型 | 风险 | 缓解措施 |
|------|------|------|----------|
| g_callbackWorker | AsioOutputWorker* | 中 | 析构时清除，回调前检查 |
| g_callbackWorkerMutex | QMutex* | 低 | 程序生命周期内有效 |

---

## 四、循环引用风险

### 评估

1. **Qt 信号槽连接**: 使用 Qt::AutoConnection，不会导致循环引用
2. **QObject 父子关系**: 单向父子关系，无循环引用风险
3. **Worker 线程**: Worker 通过 QThread::finished 信号释放，无循环引用

### 结论

未发现循环引用风险。

---

## 五、异常安全

### 评估

1. **Qt 代码**: 不使用 C++ 异常，使用信号/槽错误处理
2. **COM 代码**: 使用 HRESULT 错误码，不使用 C++ 异常
3. **ASIO 代码**: 使用 SEH 捕获驱动崩溃，不使用 C++ 异常
4. **FFmpeg 代码**: 使用返回值错误码，不使用 C++ 异常

### 结论

项目不使用 C++ 异常，异常安全风险极低。

---

## 六、内存管理建议

### 已有的良好实践

1. **Qt 对象树**: 广泛使用 Qt 对象树管理对象生命周期
2. **SafeRelease 模式**: COM 对象使用统一的 SafeRelease 函数
3. **deleteLater**: Worker 对象通过 QThread::finished 信号延迟释放
4. **quarantineBufferDevice**: 隔离旧 buffer 防止并发访问

### 潜在改进点

1. **Worker 指针**: 可以考虑使用 QPointer 或 std::unique_ptr
2. **PcmSeekCache**: 可以考虑使用 std::unique_ptr
3. **全局指针**: g_callbackWorker 可以考虑使用 QPointer
4. **HANDLE**: 可以考虑使用 RAII 封装

### 总体评估

**内存管理风险: 低**

项目使用了 Qt 对象树、SafeRelease 模式、deleteLater 等良好的内存管理实践。主要风险集中在 BlockingQueuedConnection 的死锁场景，但已有缓解措施。
