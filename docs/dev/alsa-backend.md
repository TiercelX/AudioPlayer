# Linux ALSA 后端开发指南

本文档描述 Linux ALSA 音频后端的架构设计、实现细节和开发指南。

## 概述

ALSA (Advanced Linux Sound Architecture) 是 Linux 内核的音频子系统。本项目使用 ALSA 实现精确的位深和采样率输出，作为 Windows WASAPI 后端的 Linux 等价物。

## 文件结构

```
src/backends/alsa/
├── linuxalsaaudioplayer.h            # 主类声明
├── linuxalsaaudioplayer.cpp          # 播放控制、源管理
├── linuxalsaaudioplayer_output.cpp   # 设备枚举、格式协商
├── linuxalsaaudioplayer_state.cpp    # 状态机、错误恢复
├── alsaoutputworker.h                # ALSA 输出线程
├── alsaoutputworker.cpp              # PCM 写入、XRUN 处理
├── alsaformatnegotiator.h            # 格式候选生成
└── alsaformatnegotiator.cpp          # 格式协商逻辑
```

## 架构设计

### 双 Worker 模式

复用 WASAPI 后端的架构：

```
FfmpegDecoderWorker              AlsaOutputWorker
(解码线程)                        (输出线程)
      │                                │
      │ PCM 数据写入                    │ 从 buffer 读取
      ▼                                ▼
┌──────────────────────────────────────────┐
│            PcmStreamBuffer               │
│       (QIODevice, 线程安全)              │
└──────────────────────────────────────────┘
```

### 类层次结构

```
AudioPlayerBackend (抽象接口)
    │
    ├── FfmpegAudioPlayer          [跨平台，通用回退]
    │
    ├── WindowsWasapiAudioPlayer   [Windows 专属]
    │
    ├── WindowsAsioAudioPlayer     [Windows 专属]
    │
    └── LinuxAlsaAudioPlayer       [Linux 专属，新增]
            │
            ├── AlsaOutputWorker       (输出线程)
            ├── AlsaFormatNegotiator   (格式协商)
            ├── FfmpegDecoderWorker    (解码线程，复用)
            └── PcmStreamBuffer        (PCM 缓冲区，复用)
```

## 格式协商降级链路

参考 WASAPI 的 `exclusivePcmCandidates()` 和 `selectOutputFormat()`：

```
源格式 (96kHz / 24bit / 2ch)
│
├─ L1: hw: 设备精确匹配（独占模式）
│   ├─ 测试 S24_3LE + 96000Hz + 2ch → 成功？使用
│   ├─ 测试 S24_LE + 96000Hz + 2ch → 成功？使用
│   ├─ 测试 S32_LE + 96000Hz + 2ch → 成功？使用
│   └─ 全部失败 → L2
│
├─ L2: hw: 设备降级采样率
│   ├─ 生成采样率候选 [96000, 88200, 48000, 44100, ...]
│   ├─ 对每个采样率测试格式候选
│   └─ 找到 → 使用
│
├─ L3: plughw: 设备（自动转换）
│   ├─ 测试源格式 → plughw 自动转换
│   └─ 使用 plughw + 源格式
│
└─ L4: 回退到 FFmpeg 后端
    └─ AudioPlayerFactory 选择 FfmpegAudioPlayer
```

### 采样率候选生成

```cpp
QList<unsigned int> sampleRateCandidates(int sourceRate, bool exactMode)
{
    QList<unsigned int> candidates;

    if (exactMode && sourceRate > 0) {
        candidates << sourceRate;
    }

    // 常用采样率
    const QList<unsigned int> commonRates = {
        44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000
    };

    // 按距离排序
    for (unsigned int rate : commonRates) {
        if (!candidates.contains(rate)) {
            candidates << rate;
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [sourceRate](unsigned int a, unsigned int b) {
                  return std::abs(static_cast<int>(a) - sourceRate)
                       < std::abs(static_cast<int>(b) - sourceRate);
              });

    return candidates;
}
```

### 格式候选生成

```cpp
QList<AlsaFormatCandidate> formatCandidates(int sourceBitDepth, bool exactMode)
{
    QList<AlsaFormatCandidate> candidates;

    // 源位深精确匹配
    switch (sourceBitDepth) {
    case 16:
        candidates << {SND_PCM_FORMAT_S16_LE, QAudioFormat::Int16, 16};
        break;
    case 24:
        candidates << {SND_PCM_FORMAT_S24_3LE, QAudioFormat::Int32, 24};
        candidates << {SND_PCM_FORMAT_S24_LE, QAudioFormat::Int32, 24};
        candidates << {SND_PCM_FORMAT_S32_LE, QAudioFormat::Int32, 32};
        break;
    case 32:
        candidates << {SND_PCM_FORMAT_S32_LE, QAudioFormat::Int32, 32};
        candidates << {SND_PCM_FORMAT_FLOAT_LE, QAudioFormat::Float, 32};
        break;
    default:
        candidates << {SND_PCM_FORMAT_S32_LE, QAudioFormat::Int32, 32};
        break;
    }

    // 通用后备（非精确模式）
    if (!exactMode) {
        // 添加后备格式...
    }

    return candidates;
}
```

## 错误恢复策略

复用 WASAPI 的恢复状态机：

| 错误类型 | 恢复策略 |
|---------|---------|
| XRUN (underrun) | `snd_pcm_prepare()` + 继续写入 |
| 设备挂起 | `snd_pcm_resume()` + `snd_pcm_prepare()` |
| 设备断开 | 关闭设备 → 重新枚举 → 重建管线 |
| 格式不支持 | 降级到下一候选格式 → 重建管线 |

### XRUN 处理

```cpp
int handleXrun(snd_pcm_t *handle, int error)
{
    if (error == -EPIPE) {
        // Underrun - 数据供应不足
        qWarning() << "ALSA UNDERRUN";
        return snd_pcm_prepare(handle);
    } else if (error == -ESTRPIPE) {
        // 设备挂起
        qWarning() << "ALSA device suspended";
        while ((error = snd_pcm_resume(handle)) == -EAGAIN) {
            QThread::msleep(100);
        }
        if (error < 0) {
            return snd_pcm_prepare(handle);
        }
        return 0;
    }
    return error;
}
```

## 设备枚举

使用 `snd_device_name_hint()` 枚举系统中的 ALSA 设备：

```cpp
QList<QAudioDevice> availableOutputDevices()
{
    QList<QAudioDevice> devices;

    void **hints;
    int err = snd_device_name_hint(-1, "pcm", &hints);
    if (err < 0) {
        return devices;
    }

    for (void **n = hints; *n; n++) {
        char *name = snd_device_name_get_hint(*n, "NAME");
        char *desc = snd_device_name_get_hint(*n, "DESC");
        char *io = snd_device_name_get_hint(*n, "IOID");

        // 过滤出播放设备
        if (name && (!io || strcmp(io, "Output") == 0)) {
            // 只保留 hw: 和 plughw: 设备
            if (strncmp(name, "hw:", 3) == 0 || strncmp(name, "plughw:", 7) == 0) {
                // 构建 QAudioDevice 对象
                devices.append(/* ... */);
            }
        }

        free(name);
        free(desc);
        free(io);
    }
    snd_device_name_free_hint(hints);

    return devices;
}
```

## 位深转换

### Int32 → Int24 (packed, S24_3LE)

```cpp
void convertInt32ToInt24Packed(QByteArray &data)
{
    int samples = data.size() / 4;
    const int32_t *src = reinterpret_cast<const int32_t*>(data.constData());
    QByteArray dst(samples * 3, 0);
    uint8_t *dstPtr = reinterpret_cast<uint8_t*>(dst.data());

    for (int i = 0; i < samples; ++i) {
        int32_t sample = src[i] >> 8; // 32-bit → 24-bit
        dstPtr[i * 3 + 0] = static_cast<uint8_t>(sample & 0xFF);
        dstPtr[i * 3 + 1] = static_cast<uint8_t>((sample >> 8) & 0xFF);
        dstPtr[i * 3 + 2] = static_cast<uint8_t>((sample >> 16) & 0xFF);
    }

    data = dst;
}
```

### Int32 → Int24 (4字节容器, S24_LE)

S24_LE 是 24 位数据放在 32 位容器中，低位填充 0。直接使用 Int32 数据，ALSA 会忽略低 8 位，无需转换。

## 测试验证

### 基础播放测试

```bash
./AudioPlayer
# 选择音频文件，验证播放功能
```

### 独占模式测试

```bash
# 1. 启用独占模式
# 2. 播放高采样率音频（如 96kHz/24bit）
# 3. 验证输出格式是否精确匹配

# 查看当前 ALSA 输出格式
cat /proc/asound/card0/pcm0p/sub0/hw_params
```

### 设备切换测试

```bash
# 1. 播放过程中切换输出设备
# 2. 验证自动恢复功能
```

### 格式验证

```bash
# 使用 ALSA 工具验证输出格式
# 安装 alsa-utils
sudo apt install alsa-utils

# 查看设备支持的格式
cat /proc/asound/card0/codec#0
```

## 已知限制

- `hw:` 设备独占，不支持多应用同时播放
- 某些声卡可能不支持所有格式
- WSL 环境无法访问 `hw:` 设备
- PulseAudio/PipeWire 会接管音频，绕过独占模式

## 与 WASAPI 后端的对比

| 特性 | WASAPI 独占模式 | ALSA hw: 设备 |
|------|----------------|---------------|
| 独占访问 | 是 | 是 |
| 精确采样率 | 是 | 是 |
| 精确位深 | 是 | 是 |
| 多应用共享 | 否 | 否 |
| 错误恢复 | 自动 | 手动 |
| 缓冲区管理 | IAudioClient | snd_pcm_writei |

## 参考资料

- [ALSA 官方文档](https://www.alsa-project.org/wiki/Main_Page)
- [ALSA PCM 接口](https://www.alsa-project.org/alsa-doc/alsa-lib/pcm.html)
- [ALSA 格式列表](https://www.alsa-project.org/alsa-doc/alsa-lib/group___p_c_m.html)
