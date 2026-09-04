# Phase 4: 故障排查手册

## 1. WASAPI 问题排查

### 1.1 设备切换问题

#### 问题：切换输出设备后无声音

**可能原因**：
- 旧设备资源未完全释放
- 新设备格式协商失败
- 活动输出切换事务状态机卡住

**排查步骤**：

1. 查看日志中的设备切换事务：
```powershell
Select-String -Path logs/*.log -Pattern "ActiveOutputSwitch|beginActiveOutputSwitch|applyActiveOutputSwitch"
```

2. 检查事务状态：
```
日志关键字：
- "beginActiveOutputSwitch" - 开始切换
- "applyActiveOutputSwitch" - 应用切换
- "resetActiveOutputSwitch" - 重置切换
- "ActiveOutputSwitchPhase" - 当前阶段
```

3. 检查设备枚举：
```powershell
Select-String -Path logs/*.log -Pattern "availableOutputDevices|setOutputDeviceId"
```

**解决方案**：
- 如果事务卡在 `WaitingForInvalidation`，可能需要强制刷新：`refreshOutputConfiguration(true)`
- 如果设备格式不匹配，检查 `selectOutputFormat()` 返回的格式

相关代码：`windowswasapiaudioplayer.h:72-118`

---

#### 问题：系统默认设备改变后播放器未跟随

**可能原因**：
- `QMediaDevices` 信号未连接
- 设备变化检测定时器未触发

**排查步骤**：
```powershell
Select-String -Path logs/*.log -Pattern "handleAudioOutputsChanged|SystemDeviceChange"
```

**解决方案**：
- 检查 `m_mediaDevices` 信号连接
- 检查 `m_outputDeviceChangeTimer` 是否正常工作

---

### 1.2 独占模式问题

#### 问题：无法启用独占模式

**可能原因**：
- 设备被其他应用占用
- 设备不支持独占模式
- WASAPI Exclusive 标志未正确设置

**排查步骤**：

1. 检查独占模式状态：
```powershell
Select-String -Path logs/*.log -Pattern "exclusiveMode|ExclusiveMode"
```

2. 检查设备能力：
```powershell
Select-String -Path logs/*.log -Pattern "IAudioClient|AUDCLNT_SHAREMODE_EXCLUSIVE"
```

3. 使用 WASAPI 回环捕获工具检查：
```powershell
.\tools\WasapiLoopbackCapture.exe
```

**解决方案**：
- 确保没有其他应用在使用该设备
- 检查设备是否支持独占模式（某些 USB 设备不支持）
- 尝试使用共享模式

---

#### 问题：独占模式下音频卡顿

**可能原因**：
- 缓冲区大小不匹配
- 事件驱动模式未正确配置
- 线程优先级不足

**排查步骤**：

1. 检查缓冲区配置：
```powershell
Select-String -Path logs/*.log -Pattern "bufferDuration|periodSize|exclusiveBufferDuration"
```

2. 检查事件通知：
```powershell
Select-String -Path logs/*.log -Pattern "m_refillEvent|QWinEventNotifier"
```

**解决方案**：
- 调整 `kExclusiveBufferDuration` 常量 (`windowswasapiaudioplayer_worker_helpers.h`)
- 确保输出线程具有足够优先级

---

### 1.3 错误恢复问题

#### 问题：设备失效后无法恢复

**可能原因**：
- 错误恢复状态机未正确处理
- 会话 ID 不匹配
- 恢复重试次数耗尽

**排查步骤**：

1. 检查错误恢复流程：
```powershell
Select-String -Path logs/*.log -Pattern "scheduleOutputRecovery|outputRecovery|ErrorRecovery"
```

2. 检查会话 ID：
```powershell
Select-String -Path logs/*.log -Pattern "sessionId|m_activeDecoderSessionId"
```

**解决方案**：
- 检查 `shouldAttemptOutputRecovery()` 的判断逻辑
- 增加恢复重试次数（如果合理）
- 检查设备是否真的不可用

相关代码：`windowswasapiaudioplayer.h:144-146`

---

#### 问题：恢复后有爆音/杂音

**可能原因**：
- PCM 淡入淡出未正确应用
- 缓冲区中有残留数据
- 噪声整形参数不正确

**排查步骤**：

1. 检查音频伪影检测：
```powershell
Select-String -Path logs/*.log -Pattern "AudioArtifactMonitor|artifact|pop|click|crackle"
```

2. 检查淡入淡出：
```powershell
Select-String -Path logs/*.log -Pattern "PcmFadeIn|fadeDuration|noiseShaped"
```

**解决方案**：
- 检查 `kPcmFadeInDurationMs` 常量
- 确保恢复时清空缓冲区
- 使用 `AudioArtifactMonitor` 检测具体伪影类型

---

### 1.4 空间音频问题

#### 问题：空间音频未生效

**排查步骤**：
```powershell
Select-String -Path logs/*.log -Pattern "SpatialAudio|spatialEndpoint"
```

**解决方案**：
- 确保 Windows 版本支持空间音频
- 检查设备是否支持空间音频
- 检查 `spatialEndpointFlushEnabled()` 返回值

---

## 2. ASIO 问题排查

### 2.1 驱动忙问题

#### 问题：ASIO 驱动报告忙状态

**可能原因**：
- 其他应用正在使用 ASIO 驱动
- 驱动未正确释放
- 驱动初始化超时

**排查步骤**：

1. 检查驱动状态：
```powershell
Select-String -Path logs/*.log -Pattern "ASIO|AsioBusy|driverBusy|sessionRetry"
```

2. 检查会话探测：
```powershell
Select-String -Path logs/*.log -Pattern "AsioSessionProbe|WASAPI endpoint"
```

3. 检查驱动发现：
```powershell
Select-String -Path logs/*.log -Pattern "AsioDiscovery|registry|CLSID"
```

**解决方案**：
- 关闭其他使用 ASIO 的应用（如 DAW）
- 使用 `AsioSessionProbe` 检查驱动占用情况
- 增加重试超时时间

相关代码：
- `windowsasioaudioplayer.h:122-124` (重试状态)
- `windowsasioaudioplayer_sessionprobe.h`

---

### 2.2 回调超时问题

#### 问题：ASIO 回调未按时触发

**可能原因**：
- 驱动性能问题
- 缓冲区大小设置不当
- 系统负载过高

**排查步骤**：

1. 检查回调计数：
```powershell
Select-String -Path logs/*.log -Pattern "callbackCount|m_callbackCount"
```

2. 检查缓冲区配置：
```powershell
Select-String -Path logs/*.log -Pattern "bufferSize|preferredBufferSize"
```

**解决方案**：
- 增大 ASIO 缓冲区大小
- 提高输出线程优先级
- 检查系统 DPC 延迟

---

### 2.3 格式转换问题

#### 问题：ASIO 输出音质差/有噪声

**可能原因**：
- 量化精度不匹配
- 噪声整形参数不正确
- 通道顺序错误

**排查步骤**：

1. 检查格式协商：
```powershell
Select-String -Path logs/*.log -Pattern "AsioFormats|sampleRate|bitDepth"
```

2. 检查噪声整形：
```powershell
Select-String -Path logs/*.log -Pattern "noiseShaped|quantize"
```

3. 检查通道重排：
```powershell
Select-String -Path logs/*.log -Pattern "CreativeChannelReorder|channelOrder"
```

**解决方案**：
- 检查 `AsioFormats` 中的格式转换逻辑
- 调整 `creativeChannelReorderMode` 设置
- 确保源格式与设备格式匹配

相关代码：
- `windowsasioaudioplayer_formats.h`
- `windowsasioaudioplayer_worker.h`

---

### 2.4 驱动崩溃问题

#### 问题：ASIO 驱动导致应用崩溃

**可能原因**：
- 驱动代码有 bug
- SEH 异常未捕获
- COM 接口调用失败

**排查步骤**：

1. 检查崩溃保护：
```powershell
Select-String -Path logs/*.log -Pattern "SEH|crash|exception|safeCall"
```

2. 检查驱动初始化：
```powershell
Select-String -Path logs/*.log -Pattern "initDriver|startDriver|CLSID"
```

**解决方案**：
- 使用 `AsioUtils` 中的安全包装函数
- 更新 ASIO 驱动到最新版本
- 使用其他后端（WASAPI）

相关代码：`windowsasioaudioplayer_utils.h`

---

## 3. ALSA 问题排查

### 3.1 XRUN 问题

#### 问题：频繁出现 XRUN（缓冲区欠载/过载）

**可能原因**：
- 缓冲区太小
- 系统调度延迟
- 解码速度跟不上

**排查步骤**：

1. 检查 XRUN 计数：
```powershell
Select-String -Path logs/*.log -Pattern "XRUN|xrun|underrun|overrun"
```

2. 检查缓冲区配置：
```powershell
Select-String -Path logs/*.log -Pattern "buffer_size|period_size|snd_pcm_hw_params"
```

3. 检查解码速度：
```powershell
Select-String -Path logs/*.log -Pattern "decoderDataAvailable|backpressure"
```

**解决方案**：
- 增大缓冲区大小：
```cpp
// 在 AlsaOutputWorker 中调整
snd_pcm_hw_params_set_buffer_size_near(handle, params, &bufferSize);
```
- 提高线程优先级
- 使用实时调度策略（需要 root 权限）

相关代码：`alsaoutputworker.h`

---

### 3.2 格式协商问题

#### 问题：设备不支持请求的格式

**可能原因**：
- 设备不支持该采样率/位深
- 硬件设备格式限制
- 插件层格式转换失败

**排查步骤**：

1. 检查格式协商：
```powershell
Select-String -Path logs/*.log -Pattern "AlsaFormatNegotiator|hw:|plughw:|format"
```

2. 检查设备能力：
```powershell
Select-String -Path logs/*.log -Pattern "snd_pcm_hw_params|sampleRate|channels"
```

**解决方案**：
格式协商采用降级链路：
1. `hw:` 设备（原生格式）
2. `plughw:` 设备（自动格式转换）
3. FFmpeg 软件转换

检查 `AlsaFormatNegotiator` 实现。

相关代码：`alsaformatnegotiator.h`

---

### 3.3 设备挂起问题

#### 问题：设备进入挂起状态后无法恢复

**可能原因**：
- 设备空闲超时
- 电源管理挂起设备
- ALSA 驱动 bug

**排查步骤**：

```powershell
Select-String -Path logs/*.log -Pattern "suspend|SUSPEND|snd_pcm_resume"
```

**解决方案**：
- 定期发送静音数据保持设备活跃
- 禁用设备电源管理
- 在检测到挂起时重新打开设备

---

### 3.4 独占模式问题

#### 问题：独占模式打开失败

**可能原因**：
- `hw:` 设备被其他应用占用
- 设备不支持独占模式
- 权限不足

**排查步骤**：

```powershell
Select-String -Path logs/*.log -Pattern "exclusive|hw:|EBUSY|EBADF"
```

**解决方案**：
- 确保没有其他应用使用该设备
- 检查用户是否有设备访问权限
- 使用 `plughw:` 作为后备

---

## 4. FFmpeg 问题排查

### 4.1 解码失败

#### 问题：FFmpeg 解码进程启动失败

**可能原因**：
- ffmpeg.exe 未找到
- 命令行参数错误
- 输入文件不存在或损坏

**排查步骤**：

1. 检查 FFmpeg 路径：
```powershell
Select-String -Path logs/*.log -Pattern "locateFfmpegExecutable|ffmpeg.exe"
```

2. 检查进程启动：
```powershell
Select-String -Path logs/*.log -Pattern "QProcess|startDecoding|arguments"
```

3. 检查进程输出：
```powershell
Select-String -Path logs/*.log -Pattern "stderr|exitCode|exitStatus"
```

**解决方案**：
- 确保 ffmpeg.exe 在可执行文件同目录或 PATH 中
- 检查 `ToolLocator` 配置
- 验证输入文件完整性

---

#### 问题：解码过程中崩溃

**可能原因**：
- FFmpeg 内部错误
- 输入文件格式不支持
- 内存不足

**排查步骤**：

```powershell
Select-String -Path logs/*.log -Pattern "crash|abort|SIGSEGV|exitCode"
```

**解决方案**：
- 更新 FFmpeg 版本
- 检查输入文件格式
- 增加系统内存

---

### 4.2 格式不支持

#### 问题：某些音频格式无法播放

**可能原因**：
- FFmpeg 未编译该格式支持
- 专利限制（如 AAC）
- 需要外部库

**排查步骤**：

1. 检查 FFmpeg 支持的格式：
```powershell
ffmpeg.exe -formats
ffmpeg.exe -codecs
```

2. 检查探测结果：
```powershell
Select-String -Path logs/*.log -Pattern "probeSourceInfo|codecName"
```

**解决方案**：
- 重新编译 FFmpeg 以包含所需格式
- 使用系统包管理器安装完整版 FFmpeg
- 转换文件格式

---

### 4.3 Libav 解码器问题

#### 问题：libav 解码器初始化失败

**可能原因**：
- libav 库未正确链接
- 版本不匹配
- 缺少依赖库

**排查步骤**：

```powershell
Select-String -Path logs/*.log -Pattern "AUDIOPLAYER_LIBAV_DECODER|LibavSeekDecoder"
```

**解决方案**：
- 确保 `AUDIOPLAYER_REQUIRE_LIBAV_DECODER` 配置正确
- 重新构建 FFmpeg 音频核心
- 检查 `CMakeLists.txt` 中的库链接

相关代码：`CMakeLists.txt:182-247`

---

### 4.4 Dolby 重混问题

#### 问题：Dolby 音频播放异常

**可能原因**：
- Sidecar 文件生成失败
- 下混参数不正确
- 通道映射错误

**排查步骤**：

```powershell
Select-String -Path logs/*.log -Pattern "Dolby|sidecar|downmix|RemuxRaw"
```

**解决方案**：
- 检查 `DolbyDownmixProcessor` 配置
- 验证 sidecar 文件生成
- 检查通道布局映射

相关代码：`dolbydownmixprocessor.h`

---

## 5. 通用问题

### 5.1 内存泄漏

#### 问题：内存使用持续增长

**排查步骤**：

1. 使用 Qt 内置检测：
```cpp
// 在 main.cpp 中添加
#include <QDebug>
// Qt 会自动报告未释放的 QObject
```

2. 检查 PcmStreamBuffer：
```powershell
Select-String -Path logs/*.log -Pattern "bufferedBytes|maxSize|clear"
```

3. 检查 PcmSeekCache：
```powershell
Select-String -Path logs/*.log -Pattern "totalCachedBytes|prune|clear"
```

**常见泄漏点**：
- `QThread` 未正确退出和删除
- `QProcess` 未正确清理
- `PcmStreamBuffer` 未清空
- `PcmSeekCache` 未清理

**解决方案**：
- 确保所有 Worker 在析构时正确停止
- 使用 `QPointer` 跟踪可能被删除的对象
- 定期调用 `prunePlaybackCacheNow()`

---

### 5.2 线程死锁

#### 问题：应用挂起，无法响应

**排查步骤**：

1. 使用调试器中断并查看线程状态
2. 检查锁的获取顺序
3. 检查信号/槽连接类型

**常见死锁场景**：
- `PcmStreamBuffer` 的 `m_mutex` 被长时间持有
- 主线程等待 Worker 线程，而 Worker 线程等待主线程
- `QMutex` 重入

**解决方案**：
- 使用 `QMutex::tryLock()` 带超时
- 避免在锁内调用可能阻塞的函数
- 使用 `Qt::QueuedConnection` 进行跨线程通信

---

### 5.3 线程安全问题

#### 问题：随机崩溃或数据损坏

**排查步骤**：

1. 启用 Qt 线程安全检测：
```cpp
// 在 main.cpp 中添加
QCoreApplication::setAttribute(Qt::AA_DontCheckThreadAffinity, false);
```

2. 检查信号/槽连接：
```powershell
Select-String -Path logs/*.log -Pattern "QueuedConnection|DirectConnection"
```

**常见问题**：
- 在错误的线程访问 QObject
- 信号/槽使用了错误的连接类型
- 共享数据未加锁

**解决方案**：
- 使用 `Qt::QueuedConnection` 进行跨线程通信
- 使用 `QMutex` 保护共享数据
- 使用 `QMetaObject::invokeMethod()` 进行线程安全调用

---

### 5.4 性能问题

#### 问题：播放卡顿

**排查步骤**：

1. 检查 CPU 使用率
2. 检查解码速度：
```powershell
Select-String -Path logs/*.log -Pattern "backpressure|pendingBytes|writableBytes"
```

3. 检查输出线程负载：
```powershell
Select-String -Path logs/*.log -Pattern "XRUN|underrun|buffer"
```

**解决方案**：
- 增大缓冲区大小
- 优化解码参数
- 提高线程优先级

---

### 5.5 启动问题

#### 问题：应用启动后立即退出

**排查步骤**：

1. 检查命令行参数：
```powershell
Select-String -Path logs/*.log -Pattern "AutomationOptions|commandLine"
```

2. 检查初始化错误：
```powershell
Select-String -Path logs/*.log -Pattern "FATAL|Error|failed"
```

**解决方案**：
- 检查 `main.cpp` 中的初始化逻辑
- 确保所有依赖库可用
- 检查命令行参数解析

---

## 6. 诊断工具

### 6.1 日志分析脚本

```powershell
# 查看错误日志
.\scripts\analyze-logs.ps1 -Level Error

# 查看特定后端日志
.\scripts\analyze-logs.ps1 -Backend WASAPI

# 查看音频伪影
.\scripts\analyze-logs.ps1 -Artifacts
```

### 6.2 WASAPI 回环捕获

```powershell
# 捕获系统音频输出
.\tools\WasapiLoopbackCapture.exe -output capture.wav -duration 10
```

### 6.3 诊断报告生成

```cpp
#include "diagnosticreportbuilder.h"

DiagnosticReportBuilder builder;
QString report = builder.generateReport();
// 保存报告
QFile file("diagnostic-report.txt");
file.open(QIODevice::WriteOnly);
file.write(report.toUtf8());
```

### 6.4 自动化测试

```powershell
# 运行烟雾测试
.\scripts\smoke-test.ps1

# 运行播放测试
.\scripts\playback-test.ps1 -File test.flac
```

---

## 7. 状态追踪文件

项目使用 `docs/bug/` 目录下的文件追踪当前问题状态：

| 文件 | 内容 |
|------|------|
| `docs/bug/README.md` | 问题索引 |
| `docs/bug/wasapi-anomaly-status.md` | WASAPI 异常状态 |
| `docs/bug/asio-status.md` | ASIO 后端状态 |
| `docs/bug/alsa-status.md` | ALSA 后端状态 |
| `docs/bug/playback-cache-status.md` | 播放缓存状态 |
| `docs/bug/harness-report-status.md` | 测试框架状态 |

更新状态文件时保持事实准确，包含具体的命令/报告/日志路径。
