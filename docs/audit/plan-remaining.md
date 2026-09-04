# AudioPlayer 工程审计与加固 - 重新规划

## 已完成工作汇总

### 提交历史
```
639fb37 docs: add Phase 1-6 audit documentation
aaa75e6 refactor: worker split, ALSA seek optimization, FFmpeg volume ramping
057290d refactor: code quality improvements and test coverage
```

### 代码变更统计
| 指标 | 数值 |
|------|------|
| 新增文件 | 12个 |
| 修改文件 | 28个 |
| 新增行数 | +2924 |
| 删除行数 | -1947 |
| 净变更 | +977行 |

---

## 剩余工作 (排除macOS)

### P0: 高优先级 ✅ 已完成

#### 1. 错误消息硬编码修复 ✅
**修复**: 添加 PlaybackError 枚举 (13个错误码)，更新所有后端和UI层
**提交**: 1803b33

#### 2. applyPcmFadeIn 统一 ✅
**修复**: 提取到 pcmutils.h，两个后端都委托给共享实现
**提交**: 1803b33

#### 3. 编译验证 ✅
**结果**: AudioPlayer.exe, WasapiLoopbackCapture.exe, AudioPlayerTests.exe 全部编译通过
**修复**: WAVEFORMATEX 前向声明问题 + readInt24Sample 声明顺序
**提交**: 1803b33

### P1: 中优先级 ✅ 已完成

#### 4. WASAPI 状态转换测试 ✅
**实现**: 创建 tests/test_wasapi_states.cpp，28个测试用例
- PlaybackState 转换 (8个)
- ActiveOutputSwitchPhase 转换 (9个)
- Error Recovery (6个)
- Enum 验证 (3个)
**提交**: dccf92e

#### 5. ALSA 设备枚举改进 ✅
**实现**: 使用 snd_device_name_hint() 实现完整设备枚举
- enumerateAlsaOutputDevices() - 枚举 hw:/plughw: 设备
- probeAlsaDevicePreferredFormat() - 探测设备首选格式
- availableOutputDeviceInfos() - 返回设备信息列表
**提交**: dccf92e

#### 6. ALSA Fade 支持 ✅
**实现**: 20ms 线性渐变，与 WASAPI/FFmpeg 一致
- setVolume() - 音量渐变
- applyPcmFadeIn() - Seek 后 Fade In
- 使用 pcmutils.h 共享函数
**提交**: dccf92e

### P2: 低优先级

#### 7. WASAPI/ASIO 集成测试
**目标**: 完整播放流程测试 (需要真实硬件)
**工作量**: 大 (12-16小时)

#### 8. 性能监控仪表盘
**目标**: 实时显示音频电平、缓冲区状态、延迟
**工作量**: 中 (6-8小时)

---

## 建议执行顺序

### 第一批 (本次会话)
1. ✅ 编译验证
2. ✅ 错误消息硬编码修复
3. ✅ applyPcmFadeIn 统一

### 第二批 (下次会话)
4. WASAPI 状态转换测试
5. ALSA 设备枚举改进
6. ALSA Fade 支持

### 第三批 (后续)
7. WASAPI/ASIO 集成测试
8. 性能监控仪表盘
