# Phase 4: API 参考手册

## 1. AudioPlayerBackend (基类接口)

**文件**: `src/core/audioplayerbackend.h`

### 类描述

`AudioPlayerBackend` 是所有音频播放后端的抽象基类，定义了统一的播放控制、设备管理、状态/信号契约。所有后端必须实现此接口。

### 枚举类型

#### BackendId
```cpp
enum class BackendId {
    Ffmpeg,           // FFmpeg 跨平台后端
    WindowsWasapi,    // Windows WASAPI 后端
    WindowsAsio,      // Windows ASIO 后端
    AppleNative,      // Apple 原生后端
    AndroidNative,    // Android 原生后端
    LinuxAlsa,        // Linux ALSA 后端
};
```

#### PlaybackState
```cpp
enum class PlaybackState {
    Stopped,   // 停止状态
    Playing,   // 播放中
    Paused,    // 暂停
    Stopping,  // 正在停止（淡出中）
};
```

### 构造函数

```cpp
explicit AudioPlayerBackend(QObject *parent = nullptr);
~AudioPlayerBackend() override = default;
```

**参数**:
- `parent`: Qt 父对象，用于内存管理

### 纯虚方法（必须实现）

#### backendId()
```cpp
virtual BackendId backendId() const = 0;
```
**返回值**: 后端标识符

#### backendName()
```cpp
virtual QString backendName() const = 0;
```
**返回值**: 后端显示名称（如 "WASAPI", "ASIO"）

#### decoderName()
```cpp
virtual QString decoderName() const = 0;
```
**返回值**: 解码器名称（如 "FFmpeg (libav)", "FFmpeg (process)"）

#### setSource()
```cpp
virtual void setSource(const QString &filePath,
                       int sourceChannelCount,
                       int sourceSampleRate,
                       int sourceBitDepth,
                       const QString &sourceCodecName) = 0;
```
**参数**:
- `filePath`: 音频文件路径
- `sourceChannelCount`: 源文件声道数
- `sourceSampleRate`: 源文件采样率
- `sourceBitDepth`: 源文件位深
- `sourceCodecName`: 源文件编解码器名称

#### play()
```cpp
virtual void play() = 0;
```
开始或恢复播放。

#### pause()
```cpp
virtual void pause() = 0;
```
暂停播放。

#### stop()
```cpp
virtual void stop() = 0;
```
停止播放。

#### seek()
```cpp
virtual void seek(qint64 positionMs) = 0;
```
**参数**:
- `positionMs`: 目标位置（毫秒）

跳转到指定位置。

#### setVolume()
```cpp
virtual void setVolume(qreal volume) = 0;
```
**参数**:
- `volume`: 音量值（0.0 - 1.0）

#### availableOutputDevices()
```cpp
virtual QList<QAudioDevice> availableOutputDevices() const = 0;
```
**返回值**: 可用输出设备列表

#### outputFormat()
```cpp
virtual QAudioFormat outputFormat() const = 0;
```
**返回值**: 当前输出音频格式

#### setOutputDeviceId()
```cpp
virtual void setOutputDeviceId(const QByteArray &deviceId) = 0;
```
**参数**:
- `deviceId`: 设备 ID

设置输出设备。

### 虚方法（可选重写）

#### exclusiveModeEnabled() / setExclusiveModeEnabled()
```cpp
virtual bool exclusiveModeEnabled() const { return false; }
virtual void setExclusiveModeEnabled(bool enabled) { Q_UNUSED(enabled); }
```
独占模式控制（WASAPI/ASIO/ALSA 支持）。

#### stabilityModeEnabled() / setStabilityModeEnabled()
```cpp
virtual bool stabilityModeEnabled() const { return false; }
virtual void setStabilityModeEnabled(bool enabled) { Q_UNUSED(enabled); }
```
稳定性模式控制。

#### exactPlaybackEnabled() / setExactPlaybackEnabled()
```cpp
virtual bool exactPlaybackEnabled() const { return true; }
virtual void setExactPlaybackEnabled(bool enabled) { Q_UNUSED(enabled); }
```
精确播放模式控制。

#### refreshOutputConfiguration()
```cpp
virtual void refreshOutputConfiguration(bool force = false) { Q_UNUSED(force); }
```
**参数**:
- `force`: 是否强制刷新

刷新输出配置。

### 信号

```cpp
// 音频电平变化
void audioLevelsChanged(qreal leftLevel, qreal rightLevel);

// 错误发生
void errorOccurred(const QString &message);

// 播放完成
void finished();

// 输出格式变化
void outputFormatChanged(const QString &deviceDescription, const QAudioFormat &format);

// 输出设备选择变化
void outputDeviceSelectionChanged();

// 可用输出设备列表变化
void outputDevicesChanged();

// 播放状态变化
void playbackStateChanged(AudioPlayerBackend::PlaybackState state);

// 播放位置变化
void positionChanged(qint64 position);

// 状态消息
void statusMessage(const QString &message);
```

### 使用示例

```cpp
// 创建后端
AudioPlayerBackend *player = AudioPlayerFactory::create(
    AudioPlayerBackend::BackendId::WindowsWasapi);

// 连接信号
connect(player, &AudioPlayerBackend::playbackStateChanged, 
        [](AudioPlayerBackend::PlaybackState state) {
    qDebug() << "State changed:" << static_cast<int>(state);
});

connect(player, &AudioPlayerBackend::errorOccurred,
        [](const QString &msg) {
    qWarning() << "Error:" << msg;
});

// 设置源文件
player->setSource("/path/to/audio.flac", 2, 44100, 16, "flac");

// 播放
player->play();

// 暂停
player->pause();

// 跳转
player->seek(30000);  // 跳转到 30 秒

// 停止
player->stop();
```

---

## 2. AudioPlayerFactory

**文件**: `src/core/audioplayerfactory.h`

### 类描述

`AudioPlayerFactory` 是纯静态工具类，负责根据源文件上下文选择后端并创建对应的 `AudioPlayerBackend` 实例。

### 结构体

#### AudioPlayerSourceContext
```cpp
struct AudioPlayerSourceContext {
    QString filePath;          // 源文件路径
    QString codecName;         // 编解码器名称
    int sourceChannelCount = 0; // 声道数
};
```

#### AudioPlaybackPlan
```cpp
struct AudioPlaybackPlan {
    enum class SourceMode {
        OriginalFile,              // 使用原始文件
        RemuxRawDolbySidecar,      // 使用 Dolby 重混 sidecar
    };

    AudioPlayerBackend::BackendId backendId = AudioPlayerBackend::BackendId::Ffmpeg;
    SourceMode sourceMode = SourceMode::OriginalFile;
};
```

### 静态方法

#### buildPlaybackPlan()
```cpp
static AudioPlaybackPlan buildPlaybackPlan(const AudioPlayerSourceContext &context);
```
**参数**:
- `context`: 源文件上下文信息

**返回值**: 播放计划，包含后端选择和源模式

**说明**: 分析源文件上下文，决定使用哪个后端以及是否需要 Dolby 重混。

#### selectBackend()
```cpp
static AudioPlayerBackend::BackendId selectBackend(const AudioPlayerSourceContext &context);
```
**参数**:
- `context`: 源文件上下文信息

**返回值**: 选择的后端 ID

#### create()
```cpp
static AudioPlayerBackend *create(AudioPlayerBackend::BackendId backendId, QObject *parent = nullptr);
```
**参数**:
- `backendId`: 后端 ID
- `parent`: Qt 父对象

**返回值**: 创建的后端实例（调用者负责内存管理）

### 使用示例

```cpp
// 分析源文件
AudioPlayerSourceContext context;
context.filePath = "/path/to/dolby_atmos.mkv";
context.codecName = "truehd";
context.sourceChannelCount = 8;

// 获取播放计划
AudioPlaybackPlan plan = AudioPlayerFactory::buildPlaybackPlan(context);
qDebug() << "Selected backend:" << static_cast<int>(plan.backendId);
qDebug() << "Source mode:" << static_cast<int>(plan.sourceMode);

// 创建后端实例
AudioPlayerBackend *player = AudioPlayerFactory::create(plan.backendId);
if (!player) {
    qWarning() << "Failed to create backend";
    return;
}

// 如果需要 Dolby 重混，先准备 sidecar
if (plan.sourceMode == AudioPlaybackPlan::SourceMode::RemuxRawDolbySidecar) {
    PlaybackSourceService sourceService;
    auto resolution = sourceService.resolveForPlayback(
        context.filePath, {}, plan.sourceMode);
    player->setSource(resolution.playbackPath, ...);
} else {
    player->setSource(context.filePath, ...);
}
```

---

## 3. PlaybackSourceService

**文件**: `src/core/playbacksourceservice.h`

### 类描述

`PlaybackSourceService` 负责播放源的探测、解析、缓存管理和 sidecar（Dolby 重混）文件准备。无状态服务类，同步调用（内部启动 QProcess 调用 ffprobe/ffmpeg）。

### 结构体

#### PlaybackSourceResolution
```cpp
struct PlaybackSourceResolution {
    QString playbackPath;      // 实际播放路径（可能是 sidecar）
    AudioInfo sourceInfo;      // 源文件信息
    qint64 durationMs = 0;     // 时长（毫秒）
    QString statusMessage;     // 状态消息
    QString warningMessage;    // 警告消息
    bool usedSidecar = false;  // 是否使用了 sidecar
};
```

#### PlaybackPreparationEstimate
```cpp
struct PlaybackPreparationEstimate {
    bool usesSidecar = false;      // 是否需要 sidecar
    bool cacheExists = false;      // 缓存是否已存在
    QString cachePath;             // 缓存路径
    qint64 sourceSizeBytes = 0;    // 源文件大小
    int timeoutMs = 0;             // 预计超时时间
};
```

#### PlaybackCacheSettings
```cpp
struct PlaybackCacheSettings {
    QString cacheDirectory;                // 缓存目录
    int maxSidecars = 12;                  // 最大 sidecar 数量
    int maxSidecarAgeDays = 14;            // sidecar 最大天数
    int maxSidecarMiB = 512;               // sidecar 最大容量 (MiB)
    int maxDiagnosticAudioFiles = 4;       // 诊断音频最大文件数
    int maxDiagnosticAudioAgeDays = 2;     // 诊断音频最大天数
    int maxDiagnosticAudioMiB = 256;       // 诊断音频最大容量 (MiB)
    int maxPcmCacheMiB = 0;                // PCM 缓存最大容量 (MiB)
    bool maxPcmCacheMiBConfigured = false; // PCM 缓存容量是否已配置
    int maxPcmCacheAgeMinutes = 30;        // PCM 缓存最大分钟数
};
```

#### PlaybackCacheUsage
```cpp
struct PlaybackCacheUsage {
    qint64 totalBytes = 0;             // 总字节数
    qint64 sidecarBytes = 0;           // sidecar 字节数
    qint64 diagnosticAudioBytes = 0;   // 诊断音频字节数
    qint64 loopbackAudioBytes = 0;     // 回环音频字节数
    int totalFiles = 0;                // 总文件数
    int sidecarFiles = 0;              // sidecar 文件数
    int diagnosticAudioFiles = 0;      // 诊断音频文件数
    int loopbackAudioFiles = 0;        // 回环音频文件数
};
```

### 公共方法

#### estimatePreparation()
```cpp
PlaybackPreparationEstimate estimatePreparation(const QString &filePath,
                                                AudioPlaybackPlan::SourceMode sourceMode) const;
```
**参数**:
- `filePath`: 源文件路径
- `sourceMode`: 源模式

**返回值**: 准备估计信息

**说明**: 估算播放准备所需的时间和资源。

#### probeSourceInfo()
```cpp
AudioInfo probeSourceInfo(const QString &filePath) const;
```
**参数**:
- `filePath`: 源文件路径

**返回值**: 源文件音频信息

**说明**: 使用 ffprobe 探测源文件信息。

#### probeDuration()
```cpp
qint64 probeDuration(const QString &filePath) const;
```
**参数**:
- `filePath`: 源文件路径

**返回值**: 时长（毫秒）

#### playbackCacheRoot()
```cpp
QString playbackCacheRoot() const;
```
**返回值**: 播放缓存根目录

#### cacheSettings()
```cpp
PlaybackCacheSettings cacheSettings() const;
```
**返回值**: 当前缓存设置

#### cacheUsage()
```cpp
PlaybackCacheUsage cacheUsage() const;
```
**返回值**: 当前缓存使用情况

#### saveCacheSettings()
```cpp
void saveCacheSettings(const PlaybackCacheSettings &settings) const;
```
**参数**:
- `settings`: 缓存设置

#### prunePlaybackCacheNow()
```cpp
void prunePlaybackCacheNow() const;
```
立即清理播放缓存。

#### resolveForPlayback()
```cpp
PlaybackSourceResolution resolveForPlayback(const QString &filePath,
                                            const AudioInfo &initialSourceInfo,
                                            AudioPlaybackPlan::SourceMode sourceMode) const;
```
**参数**:
- `filePath`: 源文件路径
- `initialSourceInfo`: 初始源信息（可选）
- `sourceMode`: 源模式

**返回值**: 播放源解析结果

**说明**: 解析播放源，准备必要的 sidecar 文件。

### 使用示例

```cpp
PlaybackSourceService service;

// 探测源文件信息
AudioInfo info = service.probeSourceInfo("/path/to/audio.flac");
qDebug() << "Codec:" << info.codecName;
qDebug() << "Channels:" << info.channelCount;
qDebug() << "Sample rate:" << info.sampleRate;

// 估算准备时间
AudioPlaybackPlan::SourceMode mode = AudioPlaybackPlan::SourceMode::OriginalFile;
auto estimate = service.estimatePreparation("/path/to/audio.flac", mode);
qDebug() << "Estimated timeout:" << estimate.timeoutMs << "ms";

// 解析播放源
auto resolution = service.resolveForPlayback("/path/to/audio.flac", info, mode);
if (!resolution.warningMessage.isEmpty()) {
    qWarning() << "Warning:" << resolution.warningMessage;
}

// 使用解析结果设置播放器
player->setSource(resolution.playbackPath, info.channelCount, 
                  info.sampleRate, info.bitDepth, info.codecName);
```

---

## 4. PcmStreamBuffer

**文件**: `src/backends/ffmpeg/ffmpegpcmshared.h:102`

### 类描述

`PcmStreamBuffer` 是基于 `QIODevice` 的线程安全环形 PCM 缓冲区，用于解码线程和输出线程之间的数据传输。支持 session/generation 所有权验证，防止过期数据读写。

### 构造函数

```cpp
explicit PcmStreamBuffer(QObject *parent = nullptr);
```
**参数**:
- `parent`: Qt 父对象

### 公共方法

#### clear()
```cpp
void clear();
```
清空缓冲区。

#### discardPendingData()
```cpp
qsizetype discardPendingData();
```
**返回值**: 丢弃的字节数

丢弃所有待处理数据。

#### setDiscardWrites()
```cpp
void setDiscardWrites(bool discardWrites);
```
**参数**:
- `discardWrites`: 是否丢弃写入

设置是否丢弃后续写入（用于隔离损坏的缓冲区）。

#### isDiscardingWrites()
```cpp
bool isDiscardingWrites() const;
```
**返回值**: 是否正在丢弃写入

#### isSequential()
```cpp
bool isSequential() const override;
```
**返回值**: 始终返回 `true`（顺序访问设备）

#### setEndOfStream()
```cpp
void setEndOfStream(bool endOfStream);
```
**参数**:
- `endOfStream`: 是否到达流末尾

标记流结束。

#### endOfStream()
```cpp
bool endOfStream() const;
```
**返回值**: 是否到达流末尾

#### setMaxSize()
```cpp
void setMaxSize(qsizetype maxSize);
```
**参数**:
- `maxSize`: 最大缓冲区大小（字节）

设置缓冲区最大容量。

#### maxSize()
```cpp
qsizetype maxSize() const;
```
**返回值**: 最大缓冲区大小

#### setOwner()
```cpp
void setOwner(int sessionId, quint64 generation, const QString &source = QString());
```
**参数**:
- `sessionId`: 会话 ID
- `generation`: 代次
- `source`: 来源描述

设置缓冲区所有者（用于所有权验证）。

#### ownerSessionId()
```cpp
int ownerSessionId() const;
```
**返回值**: 所有者会话 ID

#### bufferGeneration()
```cpp
quint64 bufferGeneration() const;
```
**返回值**: 缓冲区代次

#### matchesOwner()
```cpp
bool matchesOwner(int sessionId, quint64 generation) const;
```
**参数**:
- `sessionId`: 会话 ID
- `generation`: 代次

**返回值**: 是否匹配所有者

#### isEmpty()
```cpp
bool isEmpty() const;
```
**返回值**: 缓冲区是否为空

#### bufferedBytes()
```cpp
qsizetype bufferedBytes() const;
```
**返回值**: 已缓冲字节数

#### writableBytes()
```cpp
qsizetype writableBytes() const;
```
**返回值**: 可写字节数

#### append()
```cpp
qint64 append(const QByteArray &chunk);
```
**参数**:
- `chunk`: PCM 数据块

**返回值**: 实际写入字节数

追加数据到缓冲区（无所有权检查）。

#### appendForOwner()
```cpp
qint64 appendForOwner(const QByteArray &chunk, int sessionId, quint64 generation);
```
**参数**:
- `chunk`: PCM 数据块
- `sessionId`: 会话 ID
- `generation`: 代次

**返回值**: 实际写入字节数

追加数据到缓冲区（带所有权检查）。

#### readForOwner()
```cpp
QByteArray readForOwner(qint64 maxSize, int sessionId, quint64 generation, bool *staleRead = nullptr);
```
**参数**:
- `maxSize`: 最大读取字节数
- `sessionId`: 会话 ID
- `generation`: 代次
- `staleRead`: 输出参数，指示是否读取了过期数据

**返回值**: 读取的 PCM 数据

从缓冲区读取数据（带所有权检查）。

#### bytesAvailable()
```cpp
qint64 bytesAvailable() const override;
```
**返回值**: 可用字节数

### 使用示例

```cpp
// 创建缓冲区
PcmStreamBuffer *buffer = new PcmStreamBuffer(this);
buffer->setMaxSize(1024 * 1024);  // 1MB 最大容量

// 设置所有者
int sessionId = 1;
quint64 generation = 1;
buffer->setOwner(sessionId, generation, "decoder-worker-1");

// 解码线程写入
QByteArray pcmData = decodeSomeAudio();
qint64 written = buffer->appendForOwner(pcmData, sessionId, generation);
if (written < pcmData.size()) {
    // 缓冲区满，等待输出线程消费
}

// 输出线程读取
bool staleRead = false;
QByteArray data = buffer->readForOwner(4096, sessionId, generation, &staleRead);
if (staleRead) {
    // 读取了过期数据，可能需要重新同步
    qWarning() << "Read stale data from buffer";
}

// 标记流结束
buffer->setEndOfStream(true);

// 清理
buffer->clear();
```

---

## 5. PcmSeekCache

**文件**: `src/backends/ffmpeg/pcmseekcache.h:12`

### 类描述

`PcmSeekCache` 提供 PCM 数据的 seek 缓存功能，支持内存和磁盘两种存储模式，加速重复 seek 操作。线程安全，使用 `mutable QMutex m_mutex` 保护。

### 结构体

#### Segment
```cpp
struct Segment {
    qint64 positionMs = 0;     // 位置（毫秒）
    qint64 durationMs = 0;     // 时长（毫秒）
    qsizetype byteOffset = 0;  // 字节偏移
    qsizetype byteSize = 0;    // 字节大小
    QByteArray data;           // PCM 数据
};
```

#### Hit
```cpp
struct Hit {
    qint64 positionMs = 0;     // 位置（毫秒）
    qint64 durationMs = 0;     // 时长（毫秒）
    qsizetype byteOffset = 0;  // 字节偏移
    qsizetype byteSize = 0;    // 字节大小
    bool valid = false;        // 是否有效
};
```

### 构造函数与析构函数

```cpp
PcmSeekCache();
~PcmSeekCache();
```

**禁止拷贝**:
```cpp
PcmSeekCache(const PcmSeekCache &) = delete;
PcmSeekCache &operator=(const PcmSeekCache &) = delete;
```

### 公共方法

#### initialize()
```cpp
bool initialize(const QString &sourcePath,
                const PcmStreamFormat &format,
                const QString &cacheRoot,
                int maxCacheMiB = 256,
                int maxAgeMinutes = 30);
```
**参数**:
- `sourcePath`: 源文件路径（用于生成缓存键）
- `format`: PCM 流格式
- `cacheRoot`: 缓存根目录
- `maxCacheMiB`: 最大缓存容量（MiB）
- `maxAgeMinutes`: 最大缓存年龄（分钟）

**返回值**: 是否初始化成功

**说明**: 初始化缓存，根据 `maxCacheMiB` 自动选择存储模式：
- ≤ 0: 禁用缓存
- ≤ 64: 内存模式
- > 64: 磁盘模式

#### clear()
```cpp
void clear();
```
清空缓存。

#### isInitialized()
```cpp
bool isInitialized() const;
```
**返回值**: 是否已初始化

#### writeSegment()
```cpp
void writeSegment(qint64 positionMs, const QByteArray &pcmData, const PcmStreamFormat &format);
```
**参数**:
- `positionMs`: 位置（毫秒）
- `pcmData`: PCM 数据
- `format`: PCM 流格式

写入一个缓存段。

#### findHit()
```cpp
Hit findHit(qint64 targetMs, qint64 toleranceMs = 1000) const;
```
**参数**:
- `targetMs`: 目标位置（毫秒）
- `toleranceMs`: 容差（毫秒）

**返回值**: 缓存命中结果

查找最接近目标位置的缓存段。

#### readSegment()
```cpp
QByteArray readSegment(const Hit &hit) const;
```
**参数**:
- `hit`: 缓存命中结果

**返回值**: PCM 数据

读取缓存段数据。

#### totalCachedBytes()
```cpp
qint64 totalCachedBytes() const;
```
**返回值**: 总缓存字节数

#### segmentCount()
```cpp
int segmentCount() const;
```
**返回值**: 缓存段数量

### 使用示例

```cpp
// 创建缓存
PcmSeekCache cache;

// 初始化（磁盘模式）
PcmStreamFormat format;
format.sampleRate = 44100;
format.channelCount = 2;
format.sampleEncoding = PcmSampleEncoding::Int16;

bool ok = cache.initialize(
    "/path/to/audio.flac",
    format,
    "/tmp/pcm-cache",
    256,  // 最大 256 MiB
    30    // 最大 30 分钟
);

if (!ok) {
    qWarning() << "Failed to initialize seek cache";
    return;
}

// 解码时写入缓存
QByteArray pcmData = decodeSegment(0, 5000);  // 0-5秒
cache.writeSegment(0, pcmData, format);

// Seek 时查找缓存
qint64 targetMs = 3000;  // 跳转到 3 秒
auto hit = cache.findHit(targetMs, 1000);  // 1 秒容差

if (hit.valid) {
    // 缓存命中，直接读取
    QByteArray cachedData = cache.readSegment(hit);
    qDebug() << "Cache hit at" << hit.positionMs << "ms";
} else {
    // 缓存未命中，需要重新解码
    QByteArray freshData = decodeFromPosition(targetMs);
    cache.writeSegment(targetMs, freshData, format);
}

// 查看缓存状态
qDebug() << "Cached:" << cache.totalCachedBytes() / 1024 << "KiB";
qDebug() << "Segments:" << cache.segmentCount();

// 清理
cache.clear();
```

---

## 6. 辅助类型

### PcmStreamFormat

**文件**: `src/backends/ffmpeg/ffmpegpcmshared.h:24`

```cpp
struct PcmStreamFormat {
    int sampleRate = 0;                          // 采样率
    int channelCount = 0;                        // 声道数
    PcmSampleEncoding sampleEncoding = PcmSampleEncoding::Unknown;  // 编码类型
    int validBitsPerSample = 0;                  // 有效位深

    int bitsPerSample() const;      // 每样本位数
    int bytesPerSample() const;     // 每样字节数
    int bytesPerFrame() const;      // 每帧字节数
    int effectiveValidBitsPerSample() const;  // 有效位深
    bool isValid() const;           // 是否有效
    QAudioFormat::SampleFormat qAudioSampleFormat() const;  // 转换为 Qt 格式
};
```

### PcmSampleEncoding

```cpp
enum class PcmSampleEncoding {
    Unknown,   // 未知
    UInt8,     // 无符号 8 位
    Int16,     // 有符号 16 位
    Int24,     // 有符号 24 位
    Int32,     // 有符号 32 位
    Float32,   // 32 位浮点
};
```

### AudioOutputDeviceInfo

```cpp
struct AudioOutputDeviceInfo {
    QByteArray id;              // 设备 ID
    QString description;        // 设备描述
    QAudioFormat preferredFormat;  // 首选格式
    QString transport;          // 传输类型

    bool isNull() const;        // 是否为空
};
```
