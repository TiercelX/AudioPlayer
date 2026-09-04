# WSL Ubuntu 开发环境搭建指南

本文档描述如何在 Windows 11 的 WSL2 Ubuntu 24.04 中构建和测试 AudioPlayer。

## 前提条件

- Windows 11 (Build 22000+)
- WSL2 已启用
- Ubuntu 24.04 已安装并完成初始化（用户名/密码已设置）

## 第一步：给用户添加 sudo 权限

在 Ubuntu 终端中执行（需要输入密码）：

```bash
# 将用户加入 sudo 组
sudo usermod -aG sudo $USER

# 验证
groups
```

如果 sudo 提示不在 sudoers 文件中，先用 `su -` 切换到 root，然后：

```bash
echo "$SUDO_USER ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/$SUDO_USER
exit
```

## 第二步：安装基础构建工具

```bash
sudo apt update && sudo apt install -y \
    build-essential cmake git pkg-config \
    libasound2-dev ffmpeg \
    alsa-utils gdb pipx
```

## 第三步：安装 Qt6 6.11.1

Ubuntu 24.04 自带的 Qt6 版本为 6.4.2，不满足项目要求的 6.5+。使用 aqtinstall 从 Qt 官方镜像安装，版本与 Windows 保持一致：

```bash
# 安装 aqtinstall
pipx install aqtinstall
pipx ensurepath

# 重新加载 PATH
source ~/.bashrc

# 安装 Qt6 6.11.1（与 Windows 版本一致）
aqt install-qt linux desktop 6.11.1 linux_gcc_64 -m qtmultimedia -O ~/Qt
```

> **说明**: aqtinstall 是从 Qt 官方镜像下载 SDK 的命令行工具，无需注册 Qt 账号。
> `-m qtmultimedia` 安装 Qt Multimedia 模块（项目需要）。

## 第四步：访问 Windows 项目文件

WSL 自动挂载 Windows 磁盘到 `/mnt/`：

```bash
cd /mnt/f/AI/Mimo/AudioPlayer
ls CMakeLists.txt
```

## 第五步：构建 FFmpeg 音频核心

项目随附自编译的精简版 FFmpeg（仅含音频解码器），与 Windows 版本保持一致：

```bash
cd /mnt/f/AI/Mimo/AudioPlayer
bash scripts/build-ffmpeg-audio-core-linux.sh
```

构建产物位于 `build-linux/ffmpeg-audio-core/`，包含 `ffmpeg` 和 `ffprobe`。

## 第六步：构建 AudioPlayer

```bash
cd /mnt/f/AI/Mimo/AudioPlayer
mkdir -p build-linux && cd build-linux

cmake .. -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH=~/Qt/6.11.1/gcc_64 \
    -DAUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT=/mnt/f/AI/Mimo/AudioPlayer/build-linux/ffmpeg-audio-core

cmake --build . --parallel $(nproc)
```

## 第七步：运行

```bash
cd /mnt/f/AI/Mimo/AudioPlayer/build-linux

export AUDIOPLAYER_FFMPEG_PATH=../build-linux/ffmpeg-audio-core/bin/ffmpeg
export AUDIOPLAYER_FFPROBE_PATH=../build-linux/ffmpeg-audio-core/bin/ffprobe

./AudioPlayer
```

如果通过 WSLg 运行，会自动弹出 GUI 窗口。

## 验证清单

```bash
# 工具链
cmake --version          # >= 3.19
ls ~/Qt/6.11.1/gcc_64/lib/cmake/Qt6/Qt6Config.cmake  # Qt6 存在
pkg-config --cflags --libs alsa  # ALSA 库路径

# FFmpeg 音频核心
build-linux/ffmpeg-audio-core/bin/ffmpeg -version  # 8.1.1
build-linux/ffmpeg-audio-core/bin/ffprobe -version # 8.1.1

# ALSA 设备（WSLg 虚拟设备）
aplay -l                 # 查看可用设备
speaker-test -t sine -f 440 -l 1  # 测试音频输出

# 构建产物
ls -la AudioPlayer       # 可执行文件
```

## WSL 限制

| 功能 | WSL 支持 | 说明 |
|------|----------|------|
| 编译 | ✅ | 完全支持 |
| GUI 运行 | ✅ | 通过 WSLg |
| 基本播放 | ✅ | 通过 WSLg 虚拟音频 |
| plughw: 设备 | ✅ | WSLg 提供虚拟设备 |
| hw: 独占模式 | ❌ | 无物理声卡直通 |
| 精确格式验证 | ❌ | 无法验证实际硬件输出 |
| 设备热插拔 | ❌ | WSL 无物理设备 |
| XRUN 恢复测试 | ❌ | 依赖真实硬件 |

## 快速设置脚本

也可以直接运行项目中的脚本：

```bash
bash /mnt/f/AI/Mimo/AudioPlayer/scripts/setup-wsl.sh
```

## 常见问题

### WSLg 没有音频

确认 WSLg 已启用：

```bash
cat /etc/wsl.conf
```

确保 `[wsl2]` 段有：

```ini
[wsl2]
guiApplications=true
```

### 跨文件系统性能

WSL 访问 `/mnt/` 下的 Windows 文件比访问 WSL 原生文件系统慢。

如果编译太慢，可以在 WSL 原生文件系统中构建：

```bash
mkdir -p ~/AudioPlayer-build
cd ~/AudioPlayer-build
cmake /mnt/f/AI/Mimo/AudioPlayer -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH=~/Qt/6.7.0/gcc_64
cmake --build . --parallel $(nproc)
```

### aqt 命令找不到

```bash
source ~/.bashrc
# 或
export PATH="$HOME/.local/bin:$PATH"
```

### Qt6 物理设备不支持独占模式

WSL 中 ALSA 的 `hw:` 设备不可用（无物理声卡直通），只能测试 `plughw:` 和代码逻辑正确性。独占模式精确输出验证需要裸机 Linux 环境。
