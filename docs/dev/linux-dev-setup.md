# Linux 开发环境搭建指南

本文档描述如何在 Ubuntu 上搭建 AudioPlayer 项目的开发环境。

## 系统要求

- Ubuntu 22.04 LTS 或更高版本
- 至少 4GB 内存
- 10GB 可用磁盘空间
- 音频输出设备（声卡、USB DAC 等）

> **WSL 用户**: 请参阅 [WSL 设置指南](wsl-setup.md) 了解 WSL2 特有的配置步骤和限制。

## 依赖安装

### 基础构建工具

```bash
sudo apt update
sudo apt install -y build-essential cmake git pkg-config
```

### Qt6 开发库

```bash
sudo apt install -y \
    qt6-base-dev \
    qt6-multimedia-dev
```

> **注意**: Ubuntu 24.04+ 的 Qt6 Concurrent 已包含在 `qt6-base-dev` 中，无需单独安装 `qt6-concurrent-dev`。

### ALSA 开发库

```bash
sudo apt install -y libasound2-dev
```

### FFmpeg 工具

```bash
sudo apt install -y ffmpeg
```

### 可选：PulseAudio 开发库

如果需要测试默认设备（通过 PulseAudio）：

```bash
sudo apt install -y libpulse-dev
```

### 一键安装所有依赖

```bash
sudo apt update && sudo apt install -y \
    build-essential cmake git pkg-config \
    qt6-base-dev qt6-multimedia-dev \
    libasound2-dev ffmpeg
```

## 获取代码

```bash
git clone <repo-url>
cd AudioPlayer
git checkout opencode-0528
```

## 构建

```bash
mkdir build-linux && cd build-linux
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . --parallel $(nproc)
```

## 运行

```bash
./AudioPlayer
```

## 验证 ALSA 设备

### 查看声卡信息

```bash
cat /proc/asound/cards
```

### 查看播放设备

```bash
aplay -l
```

### 查看录音设备

```bash
arecord -l
```

### 测试 ALSA 输出

```bash
# 播放测试音
speaker-test -t sine -f 440 -l 1

# 播放音频文件
aplay /usr/share/sounds/alsa/Front_Center.wav
```

### 查看设备支持的格式

```bash
# 查看 hw:0 设备的详细信息
cat /proc/asound/card0/pcm0p/sub0/hw_params

# 查看设备能力
cat /proc/asound/card0/pcm0p/sub0/info
```

## 权限配置

### 将用户添加到 audio 组

```bash
sudo usermod -aG audio $USER
```

注销并重新登录以使组更改生效。

### 验证权限

```bash
groups | grep audio
```

## 常见问题

### Qt6 找不到

```
CMake Error: Could not find a package configuration file provided by "Qt6"
```

解决方案：

```bash
# 查找 Qt6 安装路径
find /usr -name "Qt6Config.cmake" 2>/dev/null

# 设置环境变量
export CMAKE_PREFIX_PATH=/usr/lib/qt6

# 或在 cmake 命令中指定
cmake .. -DCMAKE_PREFIX_PATH=/usr/lib/qt6
```

### ALSA 找不到

```
Package alsa was not found
```

解决方案：

```bash
# 验证 pkg-config 能找到 alsa
pkg-config --cflags --libs alsa

# 如果找不到，检查安装
dpkg -l | grep libasound2-dev
```

### 权限被拒绝

```
ALSA lib pcm.c:856:(snd_pcm_open_noupdate) Unknown PCM cards.pcm.front
```

解决方案：

```bash
# 确保用户在 audio 组
sudo usermod -aG audio $USER

# 重新登录后验证
groups
```

### 声卡未检测到

```bash
# 检查内核模块
lsmod | grep snd

# 重新加载声卡驱动
sudo alsa force-reload
```

## 开发工具推荐

### IDE

- **VS Code** + C/C++ 扩展 + CMake Tools
- **Qt Creator**（如果安装了完整 Qt SDK）
- **CLion**

### 调试工具

```bash
# 安装调试工具
sudo apt install -y gdb valgrind

# 使用 gdb 调试
gdb ./AudioPlayer

# 使用 valgrind 检查内存
valgrind --leak-check=full ./AudioPlayer
```

### ALSA 调试工具

```bash
# 安装 ALSA 工具
sudo apt install -y alsa-utils

# 查看 ALSA 信息
alsa-info.sh

# 监控 ALSA 事件
alsactl monitor
```

## 环境验证清单

- [ ] `cmake --version` 显示 3.19 或更高
- [ ] `pkg-config --modversion Qt6Core` 显示 6.5 或更高
- [ ] `pkg-config --cflags --libs alsa` 正常输出
- [ ] `ffmpeg -version` 正常输出
- [ ] `aplay -l` 能看到音频设备
- [ ] `cmake .. -DCMAKE_BUILD_TYPE=Debug` 成功
- [ ] `cmake --build .` 成功编译
- [ ] `./AudioPlayer` 能启动并显示界面
