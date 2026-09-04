# Linux ALSA 后端状态

本文件跟踪 Linux ALSA 音频后端的开发进度和已知问题。

## Status refresh: 2026-06-04

- 当前状态：基础实现完成，待 Ubuntu 环境编译验证
- 证据限制：尚未在真实 Linux 环境测试
- 本文件不跟踪：WASAPI/ASIO 后端问题

## 当前焦点

- 在 Ubuntu 上编译验证
- 测试基本播放功能
- 验证独占模式格式协商

## 当前优先级顺序

1. 在 Ubuntu 上搭建开发环境
2. 编译验证代码正确性
3. 测试基本播放功能
4. 验证独占模式精确格式输出
5. 测试设备切换和错误恢复

## 当前验证基线

- 命令：`cmake --build . && ./AudioPlayer`
- 验证：播放音频文件，检查输出格式
- 已知限制：独占模式需要真实 Linux 环境

## 当前验收标准

- 能在 Ubuntu 上编译运行
- 能播放常见音频格式（MP3, FLAC, WAV）
- 独占模式下能精确输出 16/24/32 位深
- 独占模式下能精确输出目标采样率
- 格式不支持时能优雅降级

## 文件清单

| 文件 | 状态 | 说明 |
|------|------|------|
| `src/backends/alsa/linuxalsaaudioplayer.h` | ✅ 已创建 | 主类声明 |
| `src/backends/alsa/linuxalsaaudioplayer.cpp` | ✅ 已创建 | 播放控制 |
| `src/backends/alsa/linuxalsaaudioplayer_output.cpp` | ✅ 已创建 | 设备枚举、格式协商 |
| `src/backends/alsa/linuxalsaaudioplayer_state.cpp` | ✅ 已创建 | 状态机、错误恢复 |
| `src/backends/alsa/alsaoutputworker.h` | ✅ 已创建 | 输出线程声明 |
| `src/backends/alsa/alsaoutputworker.cpp` | ✅ 已创建 | 输出线程实现 |
| `src/backends/alsa/alsaformatnegotiator.h` | ✅ 已创建 | 格式协商声明 |
| `src/backends/alsa/alsaformatnegotiator.cpp` | ✅ 已创建 | 格式协商实现 |

## 依赖修改

| 文件 | 修改内容 | 状态 |
|------|----------|------|
| `src/core/audioplayerbackend.h` | 添加 `LinuxAlsa` 枚举值 | ✅ 完成 |
| `src/core/audioplayerfactory.cpp` | 添加 Linux 平台选择逻辑 | ✅ 完成 |
| `CMakeLists.txt` | 添加 ALSA 依赖和源文件 | ✅ 完成 |

## Phase 6 跨平台审计结果 (2026-06-13)

- 详细评估：`docs/audit/phase6-alsa-completeness.md`
- 完成度评估：~70% 相对 WASAPI 功能对等（本次对齐后提升）
- 已对齐：杜比裸流、PcmSeekCache、startup threshold、startup silence、warmup discard、fade-out
- 剩余差距：诊断事件日志、position anomaly detection、artifact monitoring、buffer quarantine

## 杜比裸流对齐 (2026-06-13)

- 对齐 WASAPI 后端的杜比裸流播放逻辑
- 新增 `rawInputFormatForSource()`：根据文件后缀判断裸流类型（`.mlp`/`.thd`/`.truehd` → truehd，`.eb3`/`.ec3` → eac3）
- CLI 解码器路径（`createFfmpegDecoder` 和 `seekWhilePlaying` 回退）添加 `-downmix` 和 `-f` 参数
- `startPipeline()` 添加 `rawDolbySource` 检测，自动扩容至 64 MiB
- PcmSeekCache 已实现懒初始化（见下方"播放质量对齐"）
- 修改文件：`linuxalsaaudioplayer.h`、`linuxalsaaudioplayer_state.cpp`
- WSL 编译验证通过

## 播放质量对齐 (2026-06-13)

### PcmSeekCache 初始化

- `startPipeline()` 懒初始化 PcmSeekCache，当 libav 解码器可用时创建
- 杜比内容（AC3/EAC3/TrueHD/裸流）默认 64 MiB cache 上限
- 传递给 `LibavSeekDecoderWorker::setSeekCache()`
- `setSource()` 和析构函数中清理 cache

### Startup Threshold

- `startupThresholdBytes()`：NormalStart 200ms、SeekResume 100ms、最低 32768 字节
- `AlsaOutputWorker::run()` 门控：等待缓冲区达到阈值后再写入 ALSA
- 防止启动时 underrun

### Startup Silence + Warmup Discard

- SeekResume profile：注入 8ms 静音帧 + 丢弃 8ms 初始解码帧
- NormalStart profile：无静音注入、无 warmup discard
- 三阶段处理：startup silence → warmup discard → 正常渲染

### Fade-out on Stop

- `requestStopFadeOut()`：触发 80ms 线性 fade-out（1.0→0.0）
- `applyPcmFadeOut()`：应用到 PCM 数据
- `releaseOutputResources()`：fade-out 后等待 100ms 再停止 worker

### 代码变更

| 文件 | 变更内容 |
|------|----------|
| `linuxalsaaudioplayer.h` | 添加 `startupThresholdBytes()` 方法 |
| `linuxalsaaudioplayer_state.cpp` | PcmSeekCache 初始化、startup threshold 计算、configure 参数更新、releaseOutputResources fade-out |
| `linuxalsaaudioplayer.cpp` | PcmSeekCache 清理（setSource + 析构） |
| `alsaoutputworker.h` | configure 添加 silence/warmup/threshold 参数；添加 fade-out 状态和方法 |
| `alsaoutputworker.cpp` | startup silence/warmup/fade-out 实现、run() 三阶段处理 |

## Seek 优化和输出切换 (2026-06-13)

### Seek 优化

- **快速 Seek 路径**：`seekWhilePlaying()` 使用 `snd_pcm_drop()` + `snd_pcm_prepare()` + 重新配置 hw_params，避免完全拆除线程
- **LibavSeekDecoderWorker 集成**：当 libav 解码器可用时，使用 `seekTo()` 保持解码器会话，仅重置 ALSA 设备和输出 Worker
- **FFmpeg CLI 回退**：当 libav 不可用时，重建 FFmpeg 解码器线程，使用 `-ss` 参数跳转
- **PipelineStartupProfile**：引入 `NormalStart`、`SeekResume`、`ActiveSwitchRebuild`、`ErrorRecovery` 四种启动配置
- **Paused 状态 Seek**：teardown 后使用 `SeekResume` profile 重启，复用 ALSA 设备配置

### 输出切换支持

- **ActiveOutputSwitchTransaction**：简化版事务状态机，支持 `DeviceSelection`、`OutputRefresh`、`SystemDeviceChange` 触发器
- **设备变更检测**：`handleAudioOutputsChanged()` 检测设备消失，自动停止播放；设备变更时触发 ActiveOutputSwitch
- **事务流程**：`beginActiveOutputSwitch()` → `applyActiveOutputSwitch()` → `startPipeline(ActiveSwitchRebuild)`

### 代码变更

| 文件 | 变更内容 |
|------|----------|
| `linuxalsaaudioplayer.h` | 添加 `PipelineStartupProfile`、`ActiveOutputSwitchTrigger`、`ActiveOutputSwitchPhase`、`ActiveOutputSwitchTransaction` 枚举/结构体；添加 `seekWhilePlaying()`、`reconfigureAlsaDevice()`、`beginActiveOutputSwitch()`、`applyActiveOutputSwitch()`、`resetActiveOutputSwitch()`、`isActiveOutputSwitchInProgress()` 方法 |
| `linuxalsaaudioplayer.cpp` | 优化 `seek()` 使用快速路径；`setOutputDeviceId()` 和 `refreshOutputConfiguration()` 使用 ActiveOutputSwitch；`handleAudioOutputsChanged()` 检测设备消失 |
| `linuxalsaaudioplayer_state.cpp` | 实现 `seekWhilePlaying()`、`reconfigureAlsaDevice()`、`startPipeline()` 支持 `SeekResume` profile、decoder 创建 lambda；实现 ActiveOutputSwitch 事务 |

### 待验证

- 快速 Seek 在真实 Linux 环境下的延迟表现
- `snd_pcm_drop()` + `snd_pcm_prepare()` 在不同 ALSA 驱动下的兼容性
- LibavSeekDecoderWorker `seekTo()` 与 ALSA 输出 Worker 的线程安全交互
- 设备切换时的音频连续性

## 设备枚举改进 (2026-06-13)

### snd_device_name_hint() 设备枚举

- **enumerateAlsaOutputDevices()**：使用 `snd_device_name_hint(-1, "pcm", &hints)` 枚举系统 ALSA PCM 设备
- **设备过滤**：仅保留 `hw:` 和 `plughw:` 输出设备，过滤输入设备和虚拟设备
- **设备描述**：解析 hint 的 DESC 字段，将换行替换为逗号分隔
- **传输类型**：`hw:` 设备标记为 `ALSA-hw`，`plughw:` 设备标记为 `ALSA-plughw`
- **格式探测**：`probeAlsaDevicePreferredFormat()` 使用 `SND_PCM_NONBLOCK` 打开设备，探测最大采样率、最大通道数和最佳格式（优先级：S32_LE > S24_LE > S24_3LE > S16_LE > FLOAT_LE）
- **设备验证**：`setOutputDeviceId()` 现在验证设备 ID 是否在 ALSA 设备列表中，未知 ID 回退到默认设备
- **设备变更检测**：`handleAudioOutputsChanged()` 使用 ALSA 原生设备列表而非 QMediaDevices

### 接口实现

- **availableOutputDeviceInfos()**：返回 ALSA 原生设备信息列表，包含 hw:/plughw: ID、描述、传输类型、首选格式
- **selectedOutputDeviceInfo()**：返回当前选中设备的 ALSA 原生信息
- **resolveOutputDeviceInfo()**：内部方法，根据 m_selectedOutputDeviceId 解析设备信息
- **availableOutputDevices()**：保持 QMediaDevices 抽象（QAudioDevice 无法表示 ALSA 设备）
- **selectedOutputDevice()**：保持 QAudioDevice 返回（QAudioDevice 限制）

### 代码变更

| 文件 | 变更内容 |
|------|----------|
| `linuxalsaaudioplayer.h` | 添加 `availableOutputDeviceInfos()`、`selectedOutputDeviceInfo()` override；添加 `resolveOutputDeviceInfo()`、`enumerateAlsaOutputDevices()`、`probeAlsaDevicePreferredFormat()` 私有方法 |
| `linuxalsaaudioplayer.cpp` | `setOutputDeviceId()` 增加 ALSA 设备验证；`handleAudioOutputsChanged()` 使用 ALSA 原生设备列表 |
| `linuxalsaaudioplayer_output.cpp` | 实现 `enumerateAlsaOutputDevices()`、`probeAlsaDevicePreferredFormat()`、`availableOutputDeviceInfos()`、`selectedOutputDeviceInfo()`、`resolveOutputDeviceInfo()` |

### 待验证

- `snd_device_name_hint()` 在不同 Linux 发行版的设备名称格式
- `SND_PCM_NONBLOCK` 探测在设备被占用时的行为
- PulseAudio/PipeWire 环境下 hint 返回的设备列表
- 热插拔设备的 hint 刷新时机

## WSL 构建环境 (2026-06-13)

- Ubuntu 24.04.1 LTS (Noble Numbat) on WSL2
- 依赖已确认：qt6-concurrent-dev 在 24.04 中已合并进 qt6-base-dev
- 创建 WSL 设置脚本：`scripts/setup-wsl.sh`
- 创建 WSL 搭建指南：`docs/dev/wsl-setup.md`
- 修正 linux-dev-setup.md 中的包名
- WSL 限制：无法测试 hw: 独占模式、精确格式验证、设备热插拔

## WSL Bug 修复 (2026-06-13)

### 修复内容

1. **startPipeline() 缺少 setPlaybackState(Playing)** — `linuxalsaaudioplayer_state.cpp`
   - 现象：smoke test 报告 playbackStarted=False，虽然音频实际在播放
   - 原因：startPipeline() 从未将状态设置为 Playing
   - 修复：在 `m_audioStarted = true` 之后添加 `setPlaybackState(PlaybackState::Playing)`
   - 验证：日志中 `[automation] state=Playing` 确认出现

2. **play() 忽略存储的 Seek 位置** — `linuxalsaaudioplayer.cpp`
   - 现象：Stopped 状态下 seek 后再 play，总是从 0 开始
   - 修复：`startPipeline(m_outputRecoveryPositionMs)` 替代 `startPipeline(0)`

3. **hw: → plughw: → default 回退链** — `linuxalsaaudioplayer_state.cpp`
   - 现象：exclusive hw: 失败时直接跳到 default，跳过 plughw:
   - 修复：hw: 失败后先尝试对应 plughw: 设备，再回退到 default
   - 应用于：NormalStart 和 SeekResume 两条路径

### 验证结果

- WSL 编译：通过
- 单元测试：通过
- Smoke test：`state=Playing` 正确出现在 automation 日志中
- 音频播放：audioLevel peak=0.088，positionMs 正确推进到 3000

## 单元测试 (2026-06-13)

### AlsaLogic 独立函数测试

- 提取 `rawInputFormatForSource()` 和 `startupThresholdBytes()` 到 `AlsaLogic` 命名空间
- `alsalogic.h/cpp`：纯 Qt 依赖，不包含 ALSA 头文件，可在 Windows 编译
- `linuxalsaaudioplayer_state.cpp`：委托给 `AlsaLogic::` 函数，保持原有行为
- 测试覆盖：9 个 rawInputFormat 测试 + 4 个 startupThreshold 测试
- Windows 构建 + ctest：全部通过（8 个测试套件，含 TestAlsaLogic）
- WSL 构建：AudioPlayer 和 AudioPlayerTests 均编译成功

### 代码变更

| 文件 | 变更内容 |
|------|----------|
| `src/backends/alsa/alsalogic.h` | 新建，`AlsaLogic` 命名空间声明 |
| `src/backends/alsa/alsalogic.cpp` | 新建，独立逻辑实现 |
| `linuxalsaaudioplayer_state.cpp` | 委托 rawInputFormatForSource/startupThresholdBytes |
| `tests/test_alsa_logic.h` | 新建，TestAlsaLogic 测试类声明 |
| `tests/test_alsa_logic.cpp` | 新建，13 个测试用例 |
| `tests/test_main.cpp` | 添加 TestAlsaLogic 到测试运行器 |
| `CMakeLists.txt` | AudioPlayer 和 AudioPlayerTests 添加 alsalogic 源文件 |

## 日期记录

- 2026-06-13: 提取 AlsaLogic 独立函数并添加单元测试（TestAlsaLogic），Windows + WSL 构建验证通过
- 2026-06-13: 搭建 WSL Ubuntu 24.04 构建环境，创建设置脚本和指南
- 2026-06-13: 实现 snd_device_name_hint() 设备枚举，支持 hw:/plughw: 设备区分和格式探测
- 2026-06-13: 实现 Seek 优化（快速路径 + PipelineStartupProfile）和输出切换支持（ActiveOutputSwitchTransaction）
- 2026-06-13: Phase 6 跨平台移植审计完成，生成三份审计文档
- 2026-06-13: 修复 startPipeline 状态转换、play() Seek 位置、plughw: 回退链；WSL smoke test 验证通过
- 2026-06-04: 完成基础实现，待 Ubuntu 编译验证
