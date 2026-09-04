# Phase 4: 开发者入门指南

## 1. 环境搭建

### 1.1 系统要求

| 依赖 | 版本要求 | 说明 |
|------|----------|------|
| Qt | ≥ 6.5 | 包含 Core, Widgets, Multimedia, Concurrent, Test 模块 |
| CMake | ≥ 3.19 | 构建系统 |
| MSVC | ≥ 2019 | Windows 编译器（推荐 MSVC 2022） |
| GCC | ≥ 11 | Linux 编译器 |
| FFmpeg | 自编译 | 音频核心运行时（ffmpeg.exe, ffprobe.exe, libav 库） |
| ALSA | 系统自带 | Linux 音频系统（libasound2-dev） |

### 1.2 Windows 环境搭建

#### 安装 Qt6

1. 下载 Qt 在线安装程序：https://www.qt.io/download-qt-installer
2. 选择 Qt 6.5 或更高版本
3. 选择 MSVC 2022 64-bit 组件
4. 安装路径建议：`D:\Qt` 或 `C:\Qt`

#### 安装 Visual Studio

1. 下载 Visual Studio 2022 Community
2. 安装"使用 C++ 的桌面开发"工作负载
3. 确保包含 Windows 10/11 SDK

#### 编译 FFmpeg 音频核心

```powershell
# 进入项目目录
cd F:\AI\Mimo\AudioPlayer

# 运行 FFmpeg 音频核心构建脚本
.\scripts\build-ffmpeg-audio-core.ps1 -Profile runtime-with-ffprobe -Toolchain msvc -RunBuild
```

构建完成后，FFmpeg 工具将位于：
```
build/ffmpeg-audio-core/runtime-with-ffprobe-msvc/
├── bin/
│   ├── ffmpeg.exe
│   └── ffprobe.exe
├── include/
│   ├── libavformat/
│   ├── libavcodec/
│   ├── libavutil/
│   └── libswresample/
└── lib/
    ├── avformat.lib
    ├── avcodec.lib
    ├── avutil.lib
    └── swresample.lib
```

### 1.3 Linux 环境搭建

#### Ubuntu/Debian

```bash
# 安装基础依赖
sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    qt6-base-dev \
    qt6-multimedia-dev \
    qt6-tools-dev \
    libasound2-dev \
    pkg-config

# 安装 FFmpeg 开发库（可选，用于 libav 解码器）
sudo apt install -y \
    libavformat-dev \
    libavcodec-dev \
    libavutil-dev \
    libswresample-dev
```

#### Fedora/RHEL

```bash
sudo dnf install -y \
    gcc-c++ \
    cmake \
    qt6-qtbase-devel \
    qt6-qtmultimedia-devel \
    qt6-qttools-devel \
    alsa-lib-devel \
    pkg-config

# FFmpeg 开发库
sudo dnf install -y \
    ffmpeg-devel
```

详细说明参见：`docs/dev/linux-dev-setup.md`

---

## 2. 构建步骤

### 2.1 Windows 构建

#### 使用 CMake 命令行

```powershell
# 创建构建目录
mkdir build
cd build

# 配置（自动检测 Qt 路径）
cmake .. -G "Visual Studio 17 2022" -A x64

# 或手动指定 Qt 路径
cmake .. -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_PREFIX_PATH="D:/Qt/6.5.0/msvc2022_64"

# 构建 Debug 版本
cmake --build . --config Debug

# 构建 Release 版本
cmake --build . --config Release

# 构建 RelWithDebInfo 版本（推荐用于调试）
cmake --build . --config RelWithDebInfo
```

#### 使用 CMake Presets（推荐）

项目包含 `CMakePresets.json`，可直接使用：

```powershell
# 查看可用预设
cmake --list-presets

# 使用预设配置
cmake --preset windows-msvc-debug

# 构建
cmake --build --preset windows-msvc-debug
```

#### 使用构建脚本

```powershell
# ASIO Release 构建
.\build-mimo-asio-release.cmd

# ASIO RelWithDebInfo 构建
.\build-mimo-asio-relwithdebinfo.cmd
```

### 2.2 Linux 构建

```bash
# 创建构建目录
mkdir build && cd build

# 配置
cmake .. -DCMAKE_BUILD_TYPE=Debug

# 构建
cmake --build . -j$(nproc)

# 或使用 Release 模式
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

### 2.3 运行测试

```powershell
# Windows
cd build
ctest -C Debug --output-on-failure

# 或直接运行测试可执行程序
.\Debug\AudioPlayerTests.exe
```

```bash
# Linux
cd build
ctest --output-on-failure

# 或直接运行
./AudioPlayerTests
```

---

## 3. 项目结构说明

```
AudioPlayer/
├── CMakeLists.txt              # 主 CMake 配置
├── CMakePresets.json            # CMake 预设
├── deploy_playable.cmake       # 部署脚本
├── AGENTS.md                   # AI 代理指令
├── CLAUDE.md                   # Claude 代理指令
│
├── src/                        # 源代码
│   ├── core/                   # 核心模块
│   │   ├── audioplayerbackend.h/cpp      # 后端基类
│   │   ├── audioplayerfactory.h/cpp      # 工厂类
│   │   ├── playbacksourceservice.h/cpp   # 播放源服务
│   │   └── nativeaudioplayerstubbase.h   # 原生平台桩
│   │
│   ├── backends/               # 后端实现
│   │   ├── ffmpeg/             # FFmpeg 后端
│   │   │   ├── ffmpegaudioplayer.h/cpp
│   │   │   ├── ffmpegpcmshared.h/cpp     # PcmStreamBuffer, FfmpegDecoderWorker
│   │   │   ├── libavseekdecoderworker.h/cpp
│   │   │   ├── pcmseekcache.h/cpp
│   │   │   └── dolbydownmixprocessor.h/cpp
│   │   │
│   │   ├── wasapi/             # WASAPI 后端
│   │   │   ├── windowswasapiaudioplayer.h/cpp
│   │   │   ├── windowswasapiaudioplayer_worker.h/cpp
│   │   │   └── windowswasapiaudioplayer_worker_helpers.h
│   │   │
│   │   ├── asio/               # ASIO 后端
│   │   │   ├── windowsasioaudioplayer.h/cpp
│   │   │   ├── windowsasioaudioplayer_worker.h/cpp
│   │   │   ├── windowsasioaudioplayer_discovery.h/cpp
│   │   │   ├── windowsasioaudioplayer_formats.h/cpp
│   │   │   ├── windowsasioaudioplayer_sessionprobe.h/cpp
│   │   │   ├── windowsasioaudioplayer_utils.h/cpp
│   │   │   └── asio_interface.h
│   │   │
│   │   ├── alsa/               # ALSA 后端
│   │   │   ├── linuxalsaaudioplayer.h/cpp
│   │   │   ├── alsaoutputworker.h/cpp
│   │   │   └── alsaformatnegotiator.h/cpp
│   │   │
│   │   └── shared/             # 共享组件
│   │       ├── audioutils.h
│   │       └── toollocator.h/cpp
│   │
│   ├── diagnostics/            # 诊断模块
│   │   ├── audioartifactmonitor.h/cpp
│   │   └── playerlogger.h/cpp
│   │
│   └── ui/                     # UI 模块
│       ├── main.cpp
│       ├── mainwindow.h/cpp/ui
│       ├── mainwindow_helpers.h/cpp
│       ├── mainwindow_output.cpp
│       ├── mainwindow_automation.cpp
│       ├── mainwindow_media.cpp
│       ├── mainwindow_cache.cpp
│       ├── mediainfodialog.h/cpp
│       ├── automationoptions.h/cpp
│       └── diagnosticreportbuilder.h/cpp
│
├── tests/                      # 测试代码
│   ├── test_main.cpp
│   ├── test_example.cpp
│   ├── test_pcmstreamformat.cpp
│   ├── test_volumecontrol.cpp
│   ├── test_audiobuffer.cpp
│   └── test_audioplayerfactory.cpp
│
├── tools/                      # 工具
│   └── wasapi-loopback-capture/
│       └── main.cpp
│
├── scripts/                    # 脚本
│   ├── build-ffmpeg-audio-core.ps1
│   └── ...
│
├── docs/                       # 文档
│   ├── audit/                  # 审计文档
│   ├── bug/                    # Bug 追踪
│   └── dev/                    # 开发文档
│
└── build/                      # 构建输出（gitignore）
```

---

## 4. 如何添加新后端

### 4.1 创建后端类

1. 在 `src/backends/` 下创建新目录（如 `src/backends/pulseaudio/`）

2. 创建后端头文件 `pulseaudioaudioplayer.h`：

```cpp
#ifndef PULSEAUDIOAUDIOPLAYER_H
#define PULSEAUDIOAUDIOPLAYER_H

#include "audioplayerbackend.h"
#include "ffmpegpcmshared.h"

class QThread;
class FfmpegDecoderWorker;
class PulseAudioOutputWorker;

class PulseAudioAudioPlayer : public AudioPlayerBackend
{
    Q_OBJECT

public:
    explicit PulseAudioAudioPlayer(QObject *parent = nullptr);
    ~PulseAudioAudioPlayer() override;

    // 必须实现的纯虚方法
    BackendId backendId() const override;
    QString backendName() const override;
    QString decoderName() const override;
    void setSource(const QString &filePath,
                   int sourceChannelCount,
                   int sourceSampleRate,
                   int sourceBitDepth,
                   const QString &sourceCodecName) override;
    QString source() const override;

    void play() override;
    void pause() override;
    void stop() override;
    void seek(qint64 positionMs) override;
    void setVolume(qreal volume) override;

    QList<QAudioDevice> availableOutputDevices() const override;
    QString outputDeviceDescription() const override;
    QAudioFormat outputFormat() const override;
    QAudioDevice selectedOutputDevice() const override;
    QByteArray selectedOutputDeviceId() const override;
    bool usesDefaultOutputDevice() const override;
    void setOutputDeviceId(const QByteArray &deviceId) override;

private:
    void startPipeline(qint64 startPositionMs);
    void teardownPipeline();
    void handleDecoderDataAvailable(int sessionId);
    void handleDecoderFinished(int sessionId, int exitCode, int exitStatus, const QString &stderrText);

    QString m_sourcePath;
    QThread *m_decoderThread = nullptr;
    FfmpegDecoderWorker *m_decoderWorker = nullptr;
    QThread *m_outputThread = nullptr;
    PulseAudioOutputWorker *m_outputWorker = nullptr;
    PcmStreamBuffer *m_buffer = nullptr;
    int m_decoderSessionId = 0;
    quint64 m_bufferGeneration = 0;
};

#endif // PULSEAUDIOAUDIOPLAYER_H
```

3. 实现后端类 `pulseaudioaudioplayer.cpp`

### 4.2 注册后端

在 `src/core/audioplayerfactory.cpp` 中添加：

```cpp
#include "pulseaudioaudioplayer.h"

AudioPlayerBackend *AudioPlayerFactory::create(AudioPlayerBackend::BackendId backendId, QObject *parent)
{
    switch (backendId) {
    case AudioPlayerBackend::BackendId::Ffmpeg:
        return new FfmpegAudioPlayer(parent);
    case AudioPlayerBackend::BackendId::WindowsWasapi:
        return new WindowsWasapiAudioPlayer(parent);
    // ... 其他后端 ...
    case AudioPlayerBackend::BackendId::PulseAudio:  // 新增
        return new PulseAudioAudioPlayer(parent);
    default:
        return nullptr;
    }
}
```

在 `src/core/audioplayerbackend.h` 中添加枚举值：

```cpp
enum class BackendId {
    Ffmpeg,
    WindowsWasapi,
    WindowsAsio,
    AppleNative,
    AndroidNative,
    LinuxAlsa,
    PulseAudio,  // 新增
};
```

### 4.3 更新 CMakeLists.txt

```cmake
# 在 CMakeLists.txt 中添加源文件
if(UNIX AND NOT APPLE)
    target_sources(AudioPlayer PRIVATE
        src/backends/pulseaudio/pulseaudioaudioplayer.h
        src/backends/pulseaudio/pulseaudioaudioplayer.cpp
    )
    target_include_directories(AudioPlayer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/pulseaudio
    )
    target_link_libraries(AudioPlayer PRIVATE pulse)
endif()
```

### 4.4 实现双 Worker 架构

新后端应遵循双 Worker 架构：

1. **解码线程**: 使用现有的 `FfmpegDecoderWorker` 或 `LibavSeekDecoderWorker`
2. **输出线程**: 创建新的 `PulseAudioOutputWorker`
3. **缓冲区**: 使用 `PcmStreamBuffer` 连接两个 Worker

---

## 5. 调试技巧

### 5.1 日志查看

日志文件位于 `logs/` 目录，按时间戳 + PID 命名：

```powershell
# 查看最新日志
Get-ChildItem logs/*.log | Sort-Object LastWriteTime -Descending | Select-Object -First 1

# 实时查看日志
Get-Content logs/*.log -Wait

# 搜索错误
Select-String -Path logs/*.log -Pattern "ERROR|WARN"
```

### 5.2 使用 PlayerLogger

```cpp
#include "playerlogger.h"

// 在代码中添加日志
PlayerLogger::instance()->log("ModuleName", "Starting playback");
PlayerLogger::instance()->logWarning("ModuleName", "Buffer underrun detected");
PlayerLogger::instance()->logError("ModuleName", "Failed to open device");
```

### 5.3 音频伪影检测

启用 `AudioArtifactMonitor` 检测音频伪影：

```cpp
// 在 Output Worker 中
AudioArtifactMonitor monitor;
monitor.analyzePcmBlock(data, byteCount, format, context, "output", renderContext);

if (monitor.artifactCountTotal() > 0) {
    qWarning() << "Detected" << monitor.artifactCountTotal() << "audio artifacts";
}
```

### 5.4 Qt Creator 调试

1. 打开 `CMakeLists.txt` 作为项目文件
2. 配置构建套件（Kit）为 MSVC 2022 64-bit
3. 设置断点并运行调试

### 5.5 Visual Studio 调试

1. 使用 CMake 生成 VS 解决方案
2. 打开 `.sln` 文件
3. 设置 `AudioPlayer` 为启动项目
4. 设置断点并按 F5 调试

### 5.6 常用调试宏

```cpp
// 输出当前函数名和行号
qDebug() << Q_FUNC_INFO << "reached here";

// 输出线程信息
qDebug() << "Current thread:" << QThread::currentThread();

// 输出音频格式
qDebug() << "Format:" << format.sampleRate() << "Hz"
         << format.channelCount() << "ch"
         << format.sampleFormat();
```

---

## 6. 常见问题

### Q1: CMake 找不到 Qt

**错误信息**：
```
CMake Error at CMakeLists.txt:52 (find_package):
  Could not find a package configuration file provided by "Qt6"
```

**解决方案**：
```powershell
# 方法1: 设置 CMAKE_PREFIX_PATH
cmake .. -DCMAKE_PREFIX_PATH="D:/Qt/6.5.0/msvc2022_64"

# 方法2: 设置环境变量
$env:CMAKE_PREFIX_PATH="D:/Qt/6.5.0/msvc2022_64"
cmake ..

# 方法3: 使用 Qt Creator 打开项目（自动检测）
```

### Q2: FFmpeg 音频核心未找到

**错误信息**：
```
CMake Error: AUDIOPLAYER_REQUIRE_FFMPEG_AUDIO_CORE is ON, but the self-built
FFmpeg audio-core tools are incomplete
```

**解决方案**：
```powershell
# 运行 FFmpeg 构建脚本
.\scripts\build-ffmpeg-audio-core.ps1 -Profile runtime-with-ffprobe -Toolchain msvc -RunBuild

# 或禁用要求
cmake .. -DAUDIOPLAYER_REQUIRE_FFMPEG_AUDIO_CORE=OFF
```

### Q3: ALSA 头文件未找到 (Linux)

**错误信息**：
```
fatal error: alsa/asoundlib.h: No such file or directory
```

**解决方案**：
```bash
# Ubuntu/Debian
sudo apt install libasound2-dev

# Fedora/RHEL
sudo dnf install alsa-lib-devel
```

### Q4: 链接错误 - 无法解析的外部符号

**错误信息**：
```
error LNK2019: 无法解析的外部符号 "__declspec(dllimport) public: __cdecl QAudioSink::..."
```

**解决方案**：
确保链接了正确的 Qt 模块：
```cmake
target_link_libraries(AudioPlayer PRIVATE
    Qt::Core
    Qt::Widgets
    Qt::Multimedia
    Qt::Concurrent
)
```

### Q5: 运行时找不到 Qt DLL

**解决方案**：
```powershell
# 使用 windeployqt 部署
windeployqt.exe Debug/AudioPlayer.exe

# 或将 Qt bin 目录加入 PATH
$env:PATH="D:/Qt/6.5.0/msvc2022_64/bin;$env:PATH"
```

### Q6: 测试编译失败

**错误信息**：
```
fatal error: QtTest/QTest: No such file or directory
```

**解决方案**：
```cmake
# 确保找到 Test 组件
find_package(Qt6 REQUIRED COMPONENTS Test)
target_link_libraries(AudioPlayerTests PRIVATE Qt6::Test)
```

### Q7: 如何只构建特定后端？

目前不支持单独构建特定后端。所有后端通过条件编译包含：
- Windows: FFmpeg + WASAPI + ASIO
- Linux: FFmpeg + ALSA
- macOS: FFmpeg（原生后端为桩实现）

### Q8: 如何添加新的音频格式支持？

音频格式支持由 FFmpeg 决定。要添加新格式：
1. 确保 FFmpeg 编译时包含了该格式的解码器
2. 在 `PlaybackSourceService::probeSourceInfo()` 中确保能正确探测
3. 测试播放功能

---

## 7. 开发工作流

### 7.1 推荐工作流

1. **阅读文档**: 先阅读 `docs/dev/agent-workflow.md` 了解工作流程
2. **查看代码地图**: 参考 `docs/dev/code-map.md` 了解文件结构
3. **运行测试**: 修改代码后运行 `ctest` 确保不破坏现有功能
4. **提交代码**: 使用有意义的提交信息

### 7.2 分支命名规范

- Codex 代理: `codex-*`
- Claude Code: `MiMo-*`, `DeepSeek-*`, `Claude-*`
- opencode: `opencode-MiMo-*`

### 7.3 代码风格

- 使用 Qt 命名约定
- 类名: PascalCase
- 方法名: camelCase
- 成员变量: m_ 前缀
- 常量: k 前缀或 UPPER_SNAKE_CASE

### 7.4 提交前检查清单

- [ ] 代码能编译通过
- [ ] 测试全部通过
- [ ] 没有引入新的编译警告
- [ ] 更新了相关文档
- [ ] 更新了状态追踪文件（如适用）
