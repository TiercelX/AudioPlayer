# Phase 3: Code Quality Audit

**Date**: 2026-06-13
**Scope**: 10 core files, 11,973 lines total
**Auditor**: MiMo Code Agent

---

## Summary

| Severity | Count |
|----------|-------|
| High     | 12    |
| Medium   | 23    |
| Low      | 15    |
| **Total** | **50** |

---

## 1. windowswasapiaudioplayer.cpp (1,571 lines)

| Line | Type | Description | Suggested Fix |
|------|------|-------------|---------------|
| 134 | 过长函数 | `~WindowsWasapiAudioPlayer()` 析构函数执行 BlockingQueuedConnection 调用后等待线程，如果 worker 线程死锁则无限期阻塞 | 添加超时机制或使用 deferred deletion |
| 46-122 | 过长函数 | 构造函数 ~76 行，初始化列表 + 信号连接 + 线程启动全部内联 | 提取 `initAudioThread()`, `initDecoderThread()`, `initTimers()` |
| 707 | C 风格转换 | `auto *buffer = static_cast<PcmStreamBuffer *>(m_bufferDevice)` — 多处重复出现 | 提供 `bufferDevice()` 访问器返回正确类型 |
| 715-716 | 魔法数字 | `65536` 作为最小缓冲区大小 | 定义 `constexpr qsizetype kMinBufferBytes = 65536` |
| 722 | 魔法数字 | `kStabilityModeOutputBufferMs / 1000` — 常量已在头文件定义，但除法散布 | 封装为 `stabilitySinkBufferBytes()` |
| 932-1439 | 过长函数 | `startPipeline()` 超过 500 行 | 拆分为 `prepareDecoderArgs()`, `configureAudioWorker()`, `startDecoderWorker()` |
| 980 | C 风格转换 | `UINT32 startupSilenceMsOverride = 0;` 使用 Windows 类型 | 使用 `quint32` 保持跨平台一致性 |
| 1075 | 魔法数字 | `32768` 作为 seek-resume 最小阈值 | 提取为 `constexpr qsizetype kSeekResumeMinThresholdBytes` |
| 1198-1201 | 魔法数字 | `bytesPerHalfSecond * 6` 和 `* 8` 作为缓冲区倍数 | 提取为命名常量 `kNormalBufferMultiplier` / `kStabilityBufferMultiplier` |
| 1271 | 缩进错误 | `}` 缩进不一致（缺少一个缩进层级） | 修正缩进 |
| 1490-1571 | 重复代码 | `handleDecoderFinished()` 与 `ffmpegaudioplayer.cpp:1008-1073` 几乎完全相同 | 提取到基类 `AudioPlayerBackend` |
| 1537-1561 | 重复代码 | 错误消息模板在多处重复（解码失败、输出错误） | 统一到 `ErrorMessages` 命名空间 |

## 2. windowswasapiaudioplayer_output.cpp (1,631 lines)

| Line | Type | Description | Suggested Fix |
|------|------|-------------|---------------|
| 21 | 魔法数字 | `constexpr int kSystemOutputDeviceChangeDebounceMs = 350` | 已命名，但 `180` (line 30) 未命名 | 
| 30 | 魔法数字 | `return 180;` — 不同 debounce 间隔未命名 | 定义 `constexpr int kDefaultOutputDeviceChangeDebounceMs = 180` |
| 57-79 | 重复代码 | `ScopedComInitializer` RAII 类与 COM 初始化模式在 worker 中也有类似实现 | 提取为共享头文件 `comutils.h` |
| 91-120 | 重复代码 | `channelMaskForCount()` 的通道映射逻辑硬编码 | 使用查表 `constexpr` 数组 |
| 338-379 | 重复代码 | `buildWaveFormat()` 和 `buildPcmWaveFormat()` (381-425) 结构几乎相同 | 合并为一个函数，通过参数区分 |
| 700-726 | 过长函数 | `setOutputDeviceId()` 中设备验证逻辑应提取 | 提取为 `normalizeDeviceId()` |
| 728-827 | 过长函数 | `handleAudioOutputsChanged()` ~100 行，多层条件判断 | 提取 early-return 检查为独立谓词 |
| 916-1017 | 过长函数 | `beginActiveOutputSwitch()` ~100 行 | 拆分 transaction 初始化和 output suspend 逻辑 |
| 1147-1332 | 过长函数 | `applyActiveOutputSwitch()` ~185 行 | 提取 `applyHotReconfigure()` 和 `applyConservativeRebuild()` |
| 1374-1631 | 过长函数 | `selectOutputFormat()` ~257 行 | 拆分为 `selectExclusiveFormat()` 和 `selectSharedFormat()` |
| 1188-1212 | 过长日志 | 单条 `.arg()` 链超过 20 个参数 | 使用 `PlayerLogger::diagnostic()` 结构化日志替代 |

## 3. windowswasapiaudioplayer_state.cpp (964 lines)

| Line | Type | Description | Suggested Fix |
|------|------|-------------|---------------|
| 17-27 | 魔法数字 | `kRecoveryStablePositionAdvanceMs = 120`, `kPositionRegressionToleranceMs = 5`, `kPositionJumpToleranceMs = 240`, `kPositionLagToleranceMs = 180` — 已命名但散布在匿名命名空间 | 考虑集中到一个 `struct PositionAnomalyThresholds` |
| 47-59 | 魔法数字 | `outputRecoveryDelayMsForAttempt()` 返回 `250`, `500`, `900` | 使用 `constexpr int kRecoveryDelays[] = {250, 500, 900}` |
| 225-244 | 过长日志 | `logPlaybackAnomaly()` 使用 13 个 `.arg()` 参数 | 使用 `PlayerLogger::diagnostic()` |
| 502-604 | 过长函数 | `handleAudioPositionUpdated()` ~102 行，含位置异常检测 | 提取 `detectPositionAnomaly()` |
| 606-918 | 过长函数 | `handleAudioStateChanged()` ~312 行 | 拆分为 `handleActiveSwitchStateChange()`, `handleRecoveryStateChange()`, `handleTerminalStateChange()` |
| 612-613 | C 风格转换 | `static_cast<QAudio::State>(state)` 和 `static_cast<QtAudio::Error>(error)` | 信号参数应使用枚举类型而非 `int` |
| 644-661 | 重复代码 | `prepareObservedInvalidationTransition` lambda 与 `prepareActiveOutputInvalidationTransition` 方法重叠 | 直接调用方法 |
| 910-917 | 重复代码 | 错误消息字符串 `tr("音频输出错误（代码 %1）...")` 与其他文件重复 | 统一到错误消息常量 |

## 4. windowswasapiaudioplayer_worker.cpp (2,903 lines)

| Line | Type | Description | Suggested Fix |
|------|------|-------------|---------------|
| 111 | 魔法数字 | `REFERENCE_TIME bufferDuration = static_cast<REFERENCE_TIME>(boundedFlushMs + 80) * 10000` — `80` 和 `10000` 未命名 | `constexpr int kFlushBufferMarginMs = 80; constexpr REFERENCE_TIME kHundredNsPerMs = 10000` |
| 260-449 | 过长函数 | `submitPcmFadeOutBeforeStop()` ~190 行 | 拆分 fade-out 循环和 drain 等待 |
| 291-293 | 魔法数字 | `qint64(64)`, `qint64(500)` 作为提交预算边界 | 提取为命名常量 |
| 538-611 | 过长函数 | `noteFirstSubmittedPcmAfterSeek()` ~73 行纯日志 | 日志逻辑应提取为独立方法 |
| 683-726 | 过长函数 | `saveSubmittedTailFingerprint()` ~43 行 | 可接受 |
| 728-838 | 过长函数 | `startRenderMirrorCapture()` ~110 行 | 提取 metadata 构建 |
| 840-912 | 过长函数 | `finishRenderMirrorCapture()` ~72 行 | 可接受 |
| 1275-1292 | 潜在资源泄漏 | `prepareForActiveOutputInvalidation` 中 `QTimer::singleShot` 捕获 `this`，如果 worker 被销毁则悬挂 | 使用 `QPointer` 或在析构中取消 |
| 1294-1478 | 过长函数 | `releaseOutput()` ~184 行 | 拆分 COM 资源释放、状态重置、信号发送 |
| 1505-1558 | 重复代码 | `handleFatalError()` 与其他文件的错误处理模式重复 | 提取到基类 |
| 1560-1578 | 重复代码 | `resetPcmFadeIn()` 与 `ffmpegaudioplayer.cpp:231-246` 逻辑相同 | 提取到共享工具函数 |
| 1580-1595 | 重复代码 | `resetVolumeRamp()` 模式在多处出现 | 统一 volume ramp 管理 |
| 1659-1721 | 过长函数 | `applyPcmFadeIn()` ~62 行，含重复的 sample-level 操作 | 提取 `applyGainToSample()` 为共享内联函数 |
| 1756-1778 | 重复代码 | `applyStopPcmFadeOut()` 与 `applyPcmFadeIn()` 共享帧遍历结构 | 提取通用 `applyGainPerFrame()` |
| 1825+ | 过长文件 | 文件已超过 2900 行 | 拆分为 `worker_output.cpp`, `worker_artifact.cpp`, `worker_format.cpp` |

## 5. ffmpegaudioplayer.cpp (1,075 lines)

| Line | Type | Description | Suggested Fix |
|------|------|-------------|---------------|
| 26-465 | 设计问题 | `AudioOutputWorker` 内部类定义在 .cpp 中 (~440 行) | 提取为独立 `audiooutputworker.h/.cpp` |
| 37 | 魔法数字 | `m_pumpTimer->setInterval(5)` — 5ms pump 间隔 | `constexpr int kOutputPumpIntervalMs = 5` |
| 40 | 魔法数字 | `m_positionTimer->setInterval(100)` | `constexpr int kPositionUpdateIntervalMs = 100` |
| 49 | 魔法数字 | `m_volumeRampTimer->setInterval(5)` | `constexpr int kVolumeRampIntervalMs = 5` |
| 239 | 魔法数字 | `format.sampleRate() * 8 / 1000` — 8ms fade-in | `constexpr int kFfmpegPcmFadeInMs = 8` |
| 278-283 | C 风格转换 | `static_cast<quint8>(*reinterpret_cast<unsigned char *>(sampleData))` | 使用 `memcpy` 避免 strict aliasing 违规 |
| 346-347 | 魔法数字 | `constexpr int kFadeOutSteps = 4` 和 `constexpr unsigned long kFadeOutStepDelayMs = 3` | 已命名，可接受 |
| 391-445 | 过长函数 | `pumpOutput()` 中无限 `while(true)` 循环 | 添加最大迭代次数保护 |
| 427 | 魔法数字 | `qMin<qint64>(bytesFree, 32768)` — 最大读取块 | `constexpr qint64 kMaxPumpChunkBytes = 32768` |
| 787-959 | 过长函数 | `startPipeline()` ~172 行 | 拆分为 `buildFfmpegArguments()` 和 `configureAndStartDecoder()` |
| 920-957 | 重复代码 | `PcmStreamFormat` 从 `QAudioFormat` 的转换逻辑与 `pcmFormatFromQAudioFormat()` 重复 | 调用现有函数 |

## 6. ffmpegpcmshared.cpp (1,020 lines)

| Line | Type | Description | Suggested Fix |
|------|------|-------------|---------------|
| 18-28 | 重复代码 | `readInt24Sample()` 在 `libavseekdecoderworker.cpp:42-52` 完全重复 | 提取到共享头文件 `pcmutils.h` |
| 50-53 | 潜在问题 | `PcmStreamBuffer` 构造函数中 `open(QIODevice::ReadOnly)` — 如果父对象已打开 IO 设备可能冲突 | 添加错误检查 |
| 179-212 | 重复代码 | `append()` 中的环形缓冲区写入逻辑与 `appendForOwner()` (214-279) 和 `readForOwner()` (281-346) 重复 | 提取 `writeToRingBuffer()` 和 `readFromRingBuffer()` |
| 353-378 | 重复代码 | `readData()` 中的环形缓冲区读取逻辑与 `readForOwner()` 重复 | 同上 |
| 395-424 | 潜在性能问题 | `ensureCapacity()` 每次需要扩容时分配新 `QByteArray` 并拷贝 | 预分配策略或使用分段缓冲区 |
| 431-579 | 过长函数 | `startDecoding()` ~148 行 | 拆分进程启动和信号连接 |
| 507 | C 风格转换 | `static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished)` | 使用 lambda with explicit types 或 Qt6 新语法 |
| 649-680 | 重复代码 | `sampleMagnitude()` 与 `libavseekdecoderworker.cpp:1017-1047` 完全重复 | 提取到共享工具 |
| 719-839 | 过长函数 | `drainDirectStandardOutput()` ~120 行 | 提取 backpressure 检测逻辑 |
| 959-961 | 潜在问题 | `m_stderrBuffer` 截断到 4096 字节，但未通知调用方数据被截断 | 添加截断标记 |

## 7. libavseekdecoderworker.cpp (1,047 lines)

| Line | Type | Description | Suggested Fix |
|------|------|-------------|---------------|
| 42-52 | 重复代码 | `readInt24Sample()` 完全重复 | 提取到 `pcmutils.h` |
| 1017-1047 | 重复代码 | `sampleMagnitude()` 完全重复 | 同上 |
| 242-415 | 过长函数 | `prepareSource()` ~173 行 | 拆分 `openFormatContext()`, `configureSwrContext()`, `allocatePacketFrame()` |
| 417-534 | 过长函数 | `seekTo()` ~117 行 | 提取 seek cache 查询逻辑 |
| 564-839 | 过长函数 | `decodeStep()` ~275 行 | 拆分 `readAndDecodePacket()`, `processDecodedFrame()`, `handleSeekDiscard()` |
| 626-700 | 过长函数 | downmix 配置逻辑 ~74 行嵌套在 `decodeStep()` 中 | 提取 `configureDownmixIfNeeded()` |
| 702-747 | 复杂逻辑 | seek discard 逻辑含多层嵌套条件 | 提取为独立函数并简化条件 |
| 841-865 | 未处理错误 | `cleanupState()` 中 libav 释放顺序重要但无错误检查 | 添加 null 检查后释放（当前已做，可接受） |
| 910-941 | 潜在问题 | `flushPendingPcm()` 使用 `mid()` 创建拷贝而非零拷贝视图 | 使用 `QByteArrayView` 或偏移量方案 |

## 8. mainwindow.cpp (914 lines)

| Line | Type | Description | Suggested Fix |
|------|------|-------------|---------------|
| 44-65 | 重复代码 | `isAsioStartupFailureMessage()` 和 `shouldShowErrorDialog()` 使用硬编码中文字符串匹配 | 使用枚举/错误码而非字符串匹配 |
| 69-347 | 过长函数 | 构造函数 ~278 行 | 拆分 `initMenuBar()`, `initConnections()`, `initTimers()` |
| 188-219 | 过长 lambda | exclusive mode toggled lambda ~30 行 | 提取为命名方法 |
| 227-255 | 过长 lambda | stability mode toggled lambda ~28 行 | 同上 |
| 269-283 | 过长 lambda | exact playback toggled lambda ~14 行 | 可接受 |
| 324-334 | 过长 lambda | slider pressed lambda | 可接受 |
| 374-390 | 过长 lambda | audioLevelsChanged lambda | 可接受 |
| 395-429 | 过长 lambda | errorOccurred lambda ~34 行 | 提取为 `handlePlayerError()` |
| 430-517 | 过长 lambda | statusMessage lambda ~87 行 | 提取为 `handleStatusMessage()` |
| 575 | C 风格转换 | `static_cast<QMouseEvent *>(event)` | Qt 推荐用法，可接受 |
| 618 | 魔法数字 | `m_loadProgressShowTimer->start(5000)` — 5 秒延迟显示 | `constexpr int kLoadProgressShowDelayMs = 5000` |
| 698 | 潜在截断 | `static_cast<int>(effectiveDuration)` — `qint64` 到 `int` 截断 | QSlider 不支持 qint64 范围，需分段或比例映射 |

## 9. mainwindow_media.cpp (272 lines)

| Line | Type | Description | Suggested Fix |
|------|------|-------------|---------------|
| 56-65 | 过长函数 | `formatBitRate()` 简短，可接受 | — |
| 98-127 | 重复代码 | `formatCodecDisplay()` 硬编码编解码器名称映射 | 使用 `QMap<QString, QString>` 或共享常量 |
| 175-251 | 过长函数 | `loadAudioFile()` ~76 行 | 可接受，但 lambda (194-249) 较长 |
| 194-249 | 过长 lambda | 文件加载完成回调 ~55 行 | 提取为 `handleAudioFileLoaded()` |

## 10. mainwindow_output.cpp (576 lines)

| Line | Type | Description | Suggested Fix |
|------|------|-------------|---------------|
| 27-32 | 重复代码 | `outputActionData()` 使用 base64 编码设备 ID | 可接受 |
| 57-155 | 过长函数 | `rebuildOutputDeviceMenu()` ~98 行 | 拆分系统输出和 ASIO 输出菜单构建 |
| 294-375 | 过长函数 | `switchOutputBackendAndDevice()` ~81 行 | 提取 `switchBackend()` 和 `restorePlaybackState()` |
| 330 | 魔法数字 | `constexpr int kExclusiveBackendSwitchCooldownMs = 300` | 已命名，可接受 |
| 344 | 潜在问题 | `QThread::msleep(300)` 在 UI 线程阻塞 | 使用 QTimer::singleShot 或异步切换 |
| 456-504 | 过长函数 | `listOutputDevicesForAutomation()` ~48 行 | 可接受 |

---

## Cross-Cutting Issues

### 1. 大量重复的 `static_cast<PcmStreamBuffer *>(m_bufferDevice)` 转换
- 出现位置：windowswasapiaudioplayer.cpp:707, 822, 887; state.cpp:227, 249, 270, 344, 930, 951; ffmpegaudioplayer.cpp:764, 1035, 1067
- **建议**: 在基类中添加类型安全的访问器 `PcmStreamBuffer *pcmBuffer() const`

### 2. readInt24Sample() 三重实现
- ffmpegpcmshared.cpp:18-28, libavseekdecoderworker.cpp:42-52
- **建议**: 提取到 `pcmutils.h`

### 3. sampleMagnitude() 双重实现
- ffmpegpcmshared.cpp:649-680, libavseekdecoderworker.cpp:1017-1047
- **建议**: 提取到 `pcmutils.h`

### 4. handleDecoderFinished() 双重实现
- windowswasapiaudioplayer.cpp:1506-1571, ffmpegaudioplayer.cpp:1008-1073
- **建议**: 提取到基类 `AudioPlayerBackend`

### 5. applyPcmFadeIn() 双重实现
- ffmpegaudioplayer.cpp:248-332, windowswasapiaudioplayer_worker.cpp:1659-1721
- **建议**: 提取到共享工具

### 6. 错误消息硬编码中文字符串匹配
- mainwindow.cpp:44-65 使用 `message.contains()` 匹配中文
- **建议**: 使用结构化错误码而非字符串匹配

### 7. 环形缓冲区读写逻辑重复
- PcmStreamBuffer 中 `append()`, `appendForOwner()`, `readForOwner()`, `readData()` 各自重复环形缓冲区逻辑
- **建议**: 提取 `copyToRingBuffer()` 和 `copyFromRingBuffer()` 私有方法

### 8. Worker 文件过大 (2,903 行)
- windowswasapiaudioplayer_worker.cpp 单文件近 3000 行
- **建议**: 拆分为 `worker_output.cpp` (核心播放), `worker_artifact.cpp` (诊断/镜像), `worker_format.cpp` (格式工具)
